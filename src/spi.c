/**
 * @file    spi.c
 * @brief   Universal SPI Driver — Software (bit-bang) implementation
 *
 * HOW BIT-BANG SPI WORKS:
 * ========================
 * The master manually toggles SCK, MOSI, and CS according to SPI timing.
 * For MODE 0 (most common):
 *
 *   1. CS goes LOW  (select device)
 *   2. For each bit (MSB first):
 *      a. Set MOSI to the bit value
 *      b. SCK goes HIGH  (slave samples MOSI on rising edge)
 *      c. Read MISO      (master samples MISO while SCK is HIGH)
 *      d. SCK goes LOW
 *   3. CS goes HIGH (deselect)
 *
 * CLOCK MODES affect step 2:
 *   Mode 0: SCK idle LOW,  sample on rise, shift on fall
 *   Mode 1: SCK idle LOW,  shift on rise,  sample on fall
 *   Mode 2: SCK idle HIGH, sample on fall, shift on rise
 *   Mode 3: SCK idle HIGH, shift on fall,  sample on rise
 *
 * FULL-DUPLEX:
 *   Unlike UART or I2C, SPI simultaneously shifts bits both directions.
 *   While the master clocks out a bit on MOSI, it reads a bit on MISO.
 *   This is done in the SAME clock edge — true simultaneous transfer.
 */

#include "spi.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * INTERNAL STRUCTURES
 * ========================================================================== */

/** Internal device node — stored in bus's device list */
struct spi_device_s {
    spi_device_cfg_t cfg;           /**< Device configuration */
    struct spi_bus_s *bus;          /**< Parent bus */
    bool             in_transaction;/**< CS currently asserted */
};

/** Internal bus instance */
struct spi_bus_s {
    spi_config_t        cfg;                                /**< Bus config */
    spi_state_t         state;                              /**< Current state */
    spi_stats_t         stats;                              /**< Counters */
    struct spi_device_s devices[SPI_MAX_DEVICES_PER_BUS];  /**< Device pool */
    uint8_t             device_count;                       /**< Registered devices */
    bool                initialized;
    uint32_t            half_period_us;                     /**< SCK half-period */
};

/* ============================================================================
 * STATIC POOL
 * ========================================================================== */

static struct spi_bus_s s_bus_pool[SPI_MAX_BUSES];
static bool             s_bus_used[SPI_MAX_BUSES];

/* ============================================================================
 * INLINE HAL HELPERS
 * ========================================================================== */

static inline void mosi_set(struct spi_bus_s *b, bool level)
{
    b->cfg.hal->mosi_write(level, b->cfg.hal->user_ctx);
}

static inline bool miso_get(struct spi_bus_s *b)
{
    return b->cfg.hal->miso_read(b->cfg.hal->user_ctx);
}

static inline void sck_set(struct spi_bus_s *b, bool level)
{
    b->cfg.hal->sck_write(level, b->cfg.hal->user_ctx);
}

static inline void cs_set(struct spi_bus_s *b, uint8_t cs_id, bool level)
{
    b->cfg.hal->cs_write(cs_id, level, b->cfg.hal->user_ctx);
}

static inline void delay_us(struct spi_bus_s *b, uint32_t us)
{
    if (us > 0 && b->cfg.hal->delay_us)
        b->cfg.hal->delay_us(us);
}

/* ============================================================================
 * BIT-BANG BYTE TRANSFER — the core engine
 * ========================================================================== */

/**
 * @brief Transfer one byte in software, return received byte
 *
 * Implements all 4 SPI clock modes correctly.
 * Called once per byte of every transfer.
 *
 * @param b       Bus instance
 * @param mode    SPI clock mode (0-3)
 * @param order   Bit order (MSB or LSB first)
 * @param tx      Byte to transmit on MOSI
 * @return        Byte received on MISO
 */
static uint8_t bb_transfer_byte(struct spi_bus_s *b, spi_mode_t mode,
                                 spi_bitorder_t order, uint8_t tx)
{
    uint8_t rx = 0;
    uint32_t t = b->half_period_us;

    /* Determine clock idle state and sample edge from mode */
    bool cpol = (mode == SPI_MODE_2 || mode == SPI_MODE_3); /* clock idle level */
    bool cpha = (mode == SPI_MODE_1 || mode == SPI_MODE_3); /* 0=sample leading */

    /* Set SCK to idle state before starting */
    sck_set(b, cpol);

    for (int i = 0; i < 8; i++) {
        /* Select which bit to send (MSB or LSB first) */
        uint8_t bit_pos = (order == SPI_BITORDER_MSB_FIRST) ? (7 - i) : i;
        bool    tx_bit  = (tx >> bit_pos) & 0x01;

        if (!cpha) {
            /*
             * CPHA=0: Data set up BEFORE leading clock edge
             *         Sample on leading edge (1st), shift on trailing (2nd)
             *
             *   MOSI: ──[bit]──────────────────────
             *   SCK:  ________/‾‾‾‾‾‾\____________
             *                 ^ sample here
             */
            mosi_set(b, tx_bit);
            delay_us(b, t);
            sck_set(b, !cpol);              /* Leading edge */
            delay_us(b, t);
            bool rx_bit = miso_get(b);      /* Sample MISO on leading edge */
            sck_set(b, cpol);               /* Trailing edge */

            if (order == SPI_BITORDER_MSB_FIRST)
                rx = (rx << 1) | rx_bit;
            else
                rx = (rx >> 1) | (rx_bit << 7);

        } else {
            /*
             * CPHA=1: Leading edge shifts data, trailing edge samples
             *
             *   SCK:  ___/‾‾‾‾‾‾\________________
             *          ^ shift   ^ sample here
             */
            sck_set(b, !cpol);              /* Leading edge — shift */
            mosi_set(b, tx_bit);
            delay_us(b, t);
            sck_set(b, cpol);               /* Trailing edge — sample */
            bool rx_bit = miso_get(b);
            delay_us(b, t);

            if (order == SPI_BITORDER_MSB_FIRST)
                rx = (rx << 1) | rx_bit;
            else
                rx = (rx >> 1) | (rx_bit << 7);
        }
    }

    /* Return SCK to idle */
    sck_set(b, cpol);

    return rx;
}

/* ============================================================================
 * CS TIMING HELPERS
 * ========================================================================== */

/** Convert nanoseconds to microseconds, rounding up (minimum 1µs if non-zero) */
static inline uint32_t ns_to_us(uint32_t ns)
{
    if (ns == 0) return 0;
    return (ns + 999) / 1000;
}

static void cs_assert(struct spi_device_s *dev)
{
    struct spi_bus_s *b = dev->bus;
    bool active = (dev->cfg.cs_pol == SPI_CS_ACTIVE_LOW) ? false : true;
    cs_set(b, dev->cfg.cs_id, active);
    b->stats.cs_toggles++;
    delay_us(b, ns_to_us(dev->cfg.cs_setup_ns));
}

static void cs_deassert(struct spi_device_s *dev)
{
    struct spi_bus_s *b = dev->bus;
    delay_us(b, ns_to_us(dev->cfg.cs_hold_ns));
    bool idle = (dev->cfg.cs_pol == SPI_CS_ACTIVE_LOW) ? true : false;
    cs_set(b, dev->cfg.cs_id, idle);
    b->stats.cs_toggles++;
}

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

spi_err_t spi_init(spi_bus_t *bus_out, const spi_config_t *cfg)
{
    if (!bus_out || !cfg)            return SPI_ERR_INVALID_ARG;
    if (!cfg->hal)                   return SPI_ERR_INVALID_ARG;
    if (!cfg->hal->cs_write)         return SPI_ERR_INVALID_ARG;
    if (!cfg->hal->millis)           return SPI_ERR_INVALID_ARG;

    /* Software mode needs GPIO callbacks */
    if (cfg->driver == SPI_MODE_SOFTWARE) {
        if (!cfg->hal->mosi_write ||
            !cfg->hal->miso_read  ||
            !cfg->hal->sck_write)    return SPI_ERR_INVALID_ARG;
    }

    /* Find a free slot */
    struct spi_bus_s *bus = NULL;
    for (int i = 0; i < SPI_MAX_BUSES; i++) {
        if (!s_bus_used[i]) {
            bus = &s_bus_pool[i];
            s_bus_used[i] = true;
            break;
        }
    }
    if (!bus) return SPI_ERR_PLATFORM;

    memset(bus, 0, sizeof(*bus));
    bus->cfg = *cfg;
    if (bus->cfg.timeout_ms == 0)
        bus->cfg.timeout_ms = SPI_TIMEOUT_DEFAULT_MS;

    /* Default half-period = 1µs (1 MHz) — devices override per-transfer */
    bus->half_period_us = 1;

    /* Put SCK in idle (LOW for mode 0/1) and CS HIGH (deasserted) */
    if (cfg->driver == SPI_MODE_SOFTWARE) {
        cfg->hal->sck_write(false, cfg->hal->user_ctx);
        cfg->hal->mosi_write(false, cfg->hal->user_ctx);
    }

    bus->state       = SPI_STATE_IDLE;
    bus->initialized = true;
    *bus_out         = bus;
    return SPI_OK;
}

spi_err_t spi_deinit(spi_bus_t bus)
{
    if (!bus || !bus->initialized) return SPI_ERR_NOT_INIT;

    for (int i = 0; i < SPI_MAX_BUSES; i++) {
        if (&s_bus_pool[i] == bus) {
            s_bus_used[i] = false;
            break;
        }
    }
    memset(bus, 0, sizeof(*bus));
    return SPI_OK;
}

spi_err_t spi_register_device(spi_bus_t bus, const spi_device_cfg_t *dev_cfg,
                               spi_device_t *device)
{
    if (!bus || !bus->initialized || !dev_cfg || !device)
        return SPI_ERR_INVALID_ARG;
    if (bus->device_count >= SPI_MAX_DEVICES_PER_BUS)
        return SPI_ERR_PLATFORM;

    struct spi_device_s *dev = &bus->devices[bus->device_count++];
    dev->cfg            = *dev_cfg;
    dev->bus            = bus;
    dev->in_transaction = false;

    /* Ensure CS starts deasserted */
    bool idle = (dev_cfg->cs_pol == SPI_CS_ACTIVE_LOW) ? true : false;
    cs_set(bus, dev_cfg->cs_id, idle);

    *device = dev;
    return SPI_OK;
}

/* ---------- Transaction control ---------- */

spi_err_t spi_transaction_begin(spi_device_t device)
{
    if (!device || !device->bus || !device->bus->initialized)
        return SPI_ERR_INVALID_ARG;
    if (device->bus->state == SPI_STATE_BUSY)
        return SPI_ERR_BUS_BUSY;

    /* Set half-period from device speed */
    uint32_t hz = device->cfg.speed_hz;
    if (hz == 0) hz = 1000000;
    device->bus->half_period_us = (1000000UL / hz) / 2;
    if (device->bus->half_period_us == 0)
        device->bus->half_period_us = 1;

    device->bus->state  = SPI_STATE_BUSY;
    device->in_transaction = true;
    cs_assert(device);
    return SPI_OK;
}

spi_err_t spi_transaction_end(spi_device_t device)
{
    if (!device || !device->bus) return SPI_ERR_INVALID_ARG;

    cs_deassert(device);
    device->in_transaction  = false;
    device->bus->state      = SPI_STATE_IDLE;
    device->bus->stats.transfers++;
    return SPI_OK;
}

/* ---------- Core transfer ---------- */

spi_err_t spi_transfer(spi_device_t device, const spi_xfer_t *xfer)
{
    if (!device || !xfer || xfer->len == 0) return SPI_ERR_INVALID_ARG;
    if (!device->bus->initialized)          return SPI_ERR_NOT_INIT;

    spi_mode_t     mode  = device->cfg.mode;
    spi_bitorder_t order = device->cfg.bit_order;
    struct spi_bus_s *b  = device->bus;

    for (size_t i = 0; i < xfer->len; i++) {
        uint8_t tx = xfer->tx_buf ? xfer->tx_buf[i] : 0x00;
        uint8_t rx = bb_transfer_byte(b, mode, order, tx);
        if (xfer->rx_buf) xfer->rx_buf[i] = rx;
    }

    b->stats.tx_bytes += xfer->len;
    b->stats.rx_bytes += xfer->rx_buf ? xfer->len : 0;
    return SPI_OK;
}

spi_err_t spi_write(spi_device_t device, const uint8_t *data, size_t len)
{
    if (!device || !data || len == 0) return SPI_ERR_INVALID_ARG;

    spi_err_t err;
    err = spi_transaction_begin(device);
    if (err != SPI_OK) return err;

    spi_xfer_t xfer = { .tx_buf = data, .rx_buf = NULL, .len = len };
    err = spi_transfer(device, &xfer);
    spi_transaction_end(device);
    return err;
}

spi_err_t spi_read(spi_device_t device, uint8_t *data, size_t len)
{
    if (!device || !data || len == 0) return SPI_ERR_INVALID_ARG;

    spi_err_t err;
    err = spi_transaction_begin(device);
    if (err != SPI_OK) return err;

    spi_xfer_t xfer = { .tx_buf = NULL, .rx_buf = data, .len = len };
    err = spi_transfer(device, &xfer);
    spi_transaction_end(device);
    return err;
}

/* ---------- Typed transfers ---------- */

spi_err_t spi_transfer8(spi_device_t device, uint8_t tx_byte, uint8_t *rx)
{
    if (!device) return SPI_ERR_INVALID_ARG;
    uint8_t dummy;
    spi_xfer_t xfer = {
        .tx_buf = &tx_byte,
        .rx_buf = rx ? rx : &dummy,
        .len    = 1
    };
    return spi_transfer(device, &xfer);
}

spi_err_t spi_transfer16(spi_device_t device, uint16_t tx_word, uint16_t *rx)
{
    if (!device) return SPI_ERR_INVALID_ARG;

    uint8_t tx[2] = { (uint8_t)(tx_word >> 8), (uint8_t)(tx_word & 0xFF) };
    uint8_t rxb[2] = {0};

    spi_xfer_t xfer = { .tx_buf = tx, .rx_buf = rxb, .len = 2 };
    spi_err_t err = spi_transfer(device, &xfer);
    if (err == SPI_OK && rx)
        *rx = ((uint16_t)rxb[0] << 8) | rxb[1];
    return err;
}

spi_err_t spi_transfer32(spi_device_t device, uint32_t tx_word, uint32_t *rx)
{
    if (!device) return SPI_ERR_INVALID_ARG;

    uint8_t tx[4] = {
        (uint8_t)(tx_word >> 24), (uint8_t)(tx_word >> 16),
        (uint8_t)(tx_word >> 8),  (uint8_t)(tx_word & 0xFF)
    };
    uint8_t rxb[4] = {0};

    spi_xfer_t xfer = { .tx_buf = tx, .rx_buf = rxb, .len = 4 };
    spi_err_t err = spi_transfer(device, &xfer);
    if (err == SPI_OK && rx)
        *rx = ((uint32_t)rxb[0] << 24) | ((uint32_t)rxb[1] << 16)
            | ((uint32_t)rxb[2] << 8)  |  rxb[3];
    return err;
}

/* ---------- Command helpers ---------- */

spi_err_t spi_write_cmd(spi_device_t device, uint8_t cmd,
                         const uint8_t *data, size_t len)
{
    if (!device) return SPI_ERR_INVALID_ARG;

    spi_err_t err;
    err = spi_transaction_begin(device);
    if (err != SPI_OK) return err;

    /* Send command byte */
    spi_xfer_t cmd_xfer = { .tx_buf = &cmd, .rx_buf = NULL, .len = 1 };
    err = spi_transfer(device, &cmd_xfer);
    if (err != SPI_OK) { spi_transaction_end(device); return err; }

    /* Send data bytes (if any) */
    if (data && len > 0) {
        spi_xfer_t data_xfer = { .tx_buf = data, .rx_buf = NULL, .len = len };
        err = spi_transfer(device, &data_xfer);
    }

    spi_transaction_end(device);
    return err;
}

spi_err_t spi_read_cmd(spi_device_t device, uint8_t cmd,
                        uint8_t *data, size_t len)
{
    if (!device || !data) return SPI_ERR_INVALID_ARG;

    spi_err_t err;
    err = spi_transaction_begin(device);
    if (err != SPI_OK) return err;

    spi_xfer_t cmd_xfer = { .tx_buf = &cmd, .rx_buf = NULL, .len = 1 };
    err = spi_transfer(device, &cmd_xfer);
    if (err != SPI_OK) { spi_transaction_end(device); return err; }

    spi_xfer_t rx_xfer = { .tx_buf = NULL, .rx_buf = data, .len = len };
    err = spi_transfer(device, &rx_xfer);

    spi_transaction_end(device);
    return err;
}

/* ---------- Diagnostics ---------- */

spi_state_t spi_get_state(spi_bus_t bus)
{
    if (!bus || !bus->initialized) return SPI_STATE_ERROR;
    return bus->state;
}

spi_err_t spi_get_stats(spi_bus_t bus, spi_stats_t *stats)
{
    if (!bus || !stats) return SPI_ERR_INVALID_ARG;
    *stats = bus->stats;
    return SPI_OK;
}

spi_err_t spi_reset_stats(spi_bus_t bus)
{
    if (!bus) return SPI_ERR_INVALID_ARG;
    memset(&bus->stats, 0, sizeof(bus->stats));
    return SPI_OK;
}

const char *spi_err_to_string(spi_err_t err)
{
    switch (err) {
        case SPI_OK:              return "Success";
        case SPI_ERR_INVALID_ARG: return "Invalid argument (NULL or bad value)";
        case SPI_ERR_NOT_INIT:    return "Bus not initialized";
        case SPI_ERR_BUS_BUSY:    return "Bus busy — transaction in progress";
        case SPI_ERR_TIMEOUT:     return "Transfer timed out";
        case SPI_ERR_OVERFLOW:    return "Transfer length exceeds buffer";
        case SPI_ERR_PLATFORM:    return "Platform HAL error";
        case SPI_ERR_NO_DEVICE:   return "Device not registered on bus";
        case SPI_ERR_DMA:         return "DMA error";
        case SPI_ERR_MODE:        return "Unsupported clock mode";
        default:                  return "Unknown error";
    }
}

const char *spi_version(void)
{
    return SPI_LIB_VERSION_STRING;
}
