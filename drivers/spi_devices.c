/**
 * @file    spi_devices.c
 * @brief   SPI device driver implementations
 *          W25Q128 Flash, MAX31855 Thermocouple, MCP3204 ADC, ST7735 TFT
 */

#include "spi_devices.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ============================================================================
 * HELPER MACROS
 * ========================================================================== */

#define CHECK(x) do { spi_err_t _e = (x); if (_e != SPI_OK) return _e; } while(0)

/* Simple busy-wait timeout using millis — platform provides via HAL */
/* For drivers we rely on transfer timeout in spi_transfer() */

/* ============================================================================
 * W25Q128 IMPLEMENTATION
 * ============================================================================
 *
 * W25Q128 MEMORY MAP:
 *   Total: 16MB (0x000000 – 0xFFFFFF)
 *   Pages: 65536 pages × 256 bytes
 *   Sectors: 4096 sectors × 4KB
 *   Blocks: 256 blocks × 64KB (or 512 × 32KB)
 *
 * WRITE WORKFLOW (must follow this order exactly):
 *   1. spi_write_enable()     → sets WEL bit
 *   2. Send command + address
 *   3. Send data (≤ 256 bytes, same page)
 *   4. CS HIGH               → triggers internal write cycle
 *   5. Poll BUSY bit until 0 → write complete (~3ms)
 *
 * ERASE WORKFLOW:
 *   1. spi_write_enable()
 *   2. Send erase command + address
 *   3. CS HIGH               → triggers erase
 *   4. Poll BUSY bit          → sector: up to 400ms, block: up to 2s
 */

static spi_err_t w25q128_write_enable(w25q128_t *dev)
{
    return spi_write_cmd(dev->device, W25Q128_CMD_WRITE_ENABLE, NULL, 0);
}

static spi_err_t w25q128_wait_busy(w25q128_t *dev, uint32_t timeout_ms)
{
    /* Poll status register bit 0 (BUSY) until clear */
    uint32_t attempts = timeout_ms * 10;   /* check every 0.1ms */
    while (attempts--) {
        uint8_t status;
        CHECK(spi_read_cmd(dev->device, W25Q128_CMD_READ_STATUS1, &status, 1));
        if (!(status & W25Q128_STATUS_BUSY))
            return SPI_OK;
        /* Small delay between polls — platform-specific */
    }
    return SPI_ERR_TIMEOUT;
}

spi_err_t w25q128_init(w25q128_t *dev, spi_device_t device)
{
    if (!dev || !device) return SPI_ERR_INVALID_ARG;

    dev->device = device;

    /* Wake from power-down (safe to send even if not sleeping) */
    CHECK(w25q128_wake(dev));

    /* Verify JEDEC ID */
    uint8_t  mfr;
    uint16_t did;
    CHECK(w25q128_read_id(dev, &mfr, &did));
    if (mfr != W25Q128_MFR_ID || did != W25Q128_DEV_ID)
        return SPI_ERR_PLATFORM;

    dev->initialized = true;
    return SPI_OK;
}

spi_err_t w25q128_read_id(w25q128_t *dev, uint8_t *mfr_id, uint16_t *dev_id)
{
    if (!dev || !mfr_id || !dev_id) return SPI_ERR_INVALID_ARG;

    uint8_t rx[3];
    CHECK(spi_read_cmd(dev->device, W25Q128_CMD_JEDEC_ID, rx, 3));

    *mfr_id = rx[0];
    *dev_id = ((uint16_t)rx[1] << 8) | rx[2];
    return SPI_OK;
}

bool w25q128_is_busy(w25q128_t *dev)
{
    if (!dev || !dev->initialized) return false;
    uint8_t status = 0;
    spi_read_cmd(dev->device, W25Q128_CMD_READ_STATUS1, &status, 1);
    return (status & W25Q128_STATUS_BUSY) != 0;
}

spi_err_t w25q128_read(w25q128_t *dev, uint32_t addr, uint8_t *data, size_t len)
{
    if (!dev || !dev->initialized || !data) return SPI_ERR_INVALID_ARG;
    if (addr + len > W25Q128_CAPACITY)       return SPI_ERR_OVERFLOW;

    /*
     * FAST_READ (0x0B) + 24-bit address + 1 dummy byte, then data
     * Transaction manually built to combine command+addr+dummy in one CS
     */
    spi_err_t err;
    uint8_t header[5] = {
        W25Q128_CMD_FAST_READ,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFF),
        0x00    /* dummy byte required for FAST_READ */
    };

    err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;

    spi_xfer_t hdr_xfer = { .tx_buf = header, .rx_buf = NULL, .len = 5 };
    err = spi_transfer(dev->device, &hdr_xfer);
    if (err != SPI_OK) { spi_transaction_end(dev->device); return err; }

    spi_xfer_t data_xfer = { .tx_buf = NULL, .rx_buf = data, .len = len };
    err = spi_transfer(dev->device, &data_xfer);

    spi_transaction_end(dev->device);
    return err;
}

spi_err_t w25q128_write(w25q128_t *dev, uint32_t addr,
                         const uint8_t *data, size_t len)
{
    if (!dev || !dev->initialized || !data) return SPI_ERR_INVALID_ARG;
    if (addr + len > W25Q128_CAPACITY)       return SPI_ERR_OVERFLOW;

    size_t written = 0;
    while (written < len) {
        /* Max bytes we can write in this page */
        uint32_t curr_addr  = addr + written;
        uint16_t page_off   = curr_addr % W25Q128_PAGE_SIZE;
        size_t   page_space = W25Q128_PAGE_SIZE - page_off;
        size_t   chunk      = (len - written < page_space) ? (len - written) : page_space;

        /* 1. Write Enable */
        CHECK(w25q128_write_enable(dev));

        /* 2. Page Program: command + 24-bit addr + data (all in one CS) */
        spi_err_t err;
        uint8_t header[4] = {
            W25Q128_CMD_PAGE_PROGRAM,
            (uint8_t)(curr_addr >> 16),
            (uint8_t)(curr_addr >> 8),
            (uint8_t)(curr_addr & 0xFF)
        };

        err = spi_transaction_begin(dev->device);
        if (err != SPI_OK) return err;

        spi_xfer_t hdr = { .tx_buf = header, .rx_buf = NULL, .len = 4 };
        err = spi_transfer(dev->device, &hdr);
        if (err != SPI_OK) { spi_transaction_end(dev->device); return err; }

        spi_xfer_t dat = { .tx_buf = data + written, .rx_buf = NULL, .len = chunk };
        err = spi_transfer(dev->device, &dat);
        spi_transaction_end(dev->device);
        if (err != SPI_OK) return err;

        /* 3. Wait for write cycle to complete */
        CHECK(w25q128_wait_busy(dev, W25Q128_PROG_TIMEOUT_MS));

        written += chunk;
    }
    return SPI_OK;
}

spi_err_t w25q128_erase_sector(w25q128_t *dev, uint32_t addr)
{
    if (!dev || !dev->initialized) return SPI_ERR_INVALID_ARG;

    /* Align to sector boundary */
    addr &= ~(W25Q128_SECTOR_SIZE - 1);

    CHECK(w25q128_write_enable(dev));

    uint8_t cmd[4] = {
        W25Q128_CMD_SECTOR_ERASE,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)(addr & 0xFF)
    };
    CHECK(spi_write(dev->device, cmd, 4));
    return w25q128_wait_busy(dev, W25Q128_ERASE_SECTOR_MS);
}

spi_err_t w25q128_erase_chip(w25q128_t *dev)
{
    if (!dev || !dev->initialized) return SPI_ERR_INVALID_ARG;
    CHECK(w25q128_write_enable(dev));
    /* Chip erase — no address needed */
    uint8_t cmd = W25Q128_CMD_CHIP_ERASE;
    CHECK(spi_write(dev->device, &cmd, 1));
    /* Chip erase can take up to 200 seconds */
    return w25q128_wait_busy(dev, 210000);
}

spi_err_t w25q128_sleep(w25q128_t *dev)
{
    if (!dev || !dev->initialized) return SPI_ERR_INVALID_ARG;
    return spi_write_cmd(dev->device, W25Q128_CMD_POWER_DOWN, NULL, 0);
}

spi_err_t w25q128_wake(w25q128_t *dev)
{
    if (!dev) return SPI_ERR_INVALID_ARG;
    spi_err_t err = spi_write_cmd(dev->device, W25Q128_CMD_RELEASE_PD, NULL, 0);
    /* Wait 3µs for device to wake — handled by CS setup time if configured */
    return err;
}

/* ============================================================================
 * MAX31855 IMPLEMENTATION
 * ============================================================================
 *
 * MAX31855 32-BIT FRAME FORMAT:
 *
 *  Bit 31–18: Thermocouple temperature (14-bit signed, 0.25°C/LSB, MSB first)
 *  Bit 17:    Reserved
 *  Bit 16:    Fault flag (1 = any fault present)
 *  Bit 15–4:  Internal (cold junction) temperature (12-bit signed, 0.0625°C/LSB)
 *  Bit 3:     Reserved
 *  Bit 2:     SCV fault (short to VCC)
 *  Bit 1:     SCG fault (short to GND)
 *  Bit 0:     OC fault  (open circuit)
 *
 * READING IS READ-ONLY: No MOSI data needed. Just clock 32 bits.
 * The device always transmits its last conversion (auto-converts every ~100ms).
 */

spi_err_t max31855_init(max31855_t *dev, spi_device_t device)
{
    if (!dev || !device) return SPI_ERR_INVALID_ARG;
    dev->device      = device;
    dev->initialized = true;
    return SPI_OK;
}

spi_err_t max31855_read(max31855_t *dev, max31855_reading_t *reading)
{
    if (!dev || !dev->initialized || !reading) return SPI_ERR_INVALID_ARG;

    /* Read 32-bit frame — MOSI is don't-care, send 0x00000000 */
    uint32_t raw = 0;
    CHECK(spi_transaction_begin(dev->device));
    spi_err_t err = spi_transfer32(dev->device, 0x00000000, &raw);
    spi_transaction_end(dev->device);
    if (err != SPI_OK) return err;

    /*
     * Parse thermocouple temperature [31:18] — 14-bit signed
     * Value is in units of 0.25°C
     */
    int32_t tc_raw = (int32_t)(raw >> 18);
    if (tc_raw & 0x2000)                    /* sign extend from bit 13 */
        tc_raw |= (int32_t)0xFFFFC000;
    reading->thermocouple_c4 = tc_raw;      /* divide by 4 for °C */

    /*
     * Parse cold junction temperature [15:4] — 12-bit signed
     * Value is in units of 0.0625°C
     */
    int32_t cj_raw = (int32_t)((raw >> 4) & 0x0FFF);
    if (cj_raw & 0x0800)                    /* sign extend from bit 11 */
        cj_raw |= (int32_t)0xFFFFF000;
    reading->junction_c16 = cj_raw;         /* divide by 16 for °C */

    /* Parse fault bits */
    reading->fault = (uint8_t)(raw & 0x07);
    if (raw & MAX31855_FAULT_ANY)
        reading->fault |= (uint8_t)(MAX31855_FAULT_ANY & 0xFF);

    reading->valid = (reading->fault == 0);
    return SPI_OK;
}

int32_t max31855_read_temp_c100(max31855_t *dev)
{
    max31855_reading_t r;
    if (max31855_read(dev, &r) != SPI_OK || !r.valid)
        return INT32_MIN;

    /*
     * thermocouple_c4 is temp × 4 (0.25°C LSB)
     * Convert to temp × 100:
     *   temp_c100 = thermocouple_c4 * 100 / 4 = thermocouple_c4 * 25
     */
    return r.thermocouple_c4 * 25;
}

/* ============================================================================
 * MCP3204 IMPLEMENTATION
 * ============================================================================
 *
 * MCP3204 SPI COMMUNICATION PROTOCOL:
 *
 * The MCP3204 uses a non-standard bit-banged protocol within SPI frames.
 * A conversion is initiated by a specific bit sequence over 3 bytes:
 *
 * Byte 0 TX: [0][0][0][0][0][0][0][1]     ← start bit in bit 0
 * Byte 1 TX: [SGL][D2][D1][D0][X][X][X][X] ← SGL=1 single-ended, D2:D0=channel
 * Byte 2 TX: [X][X][X][X][X][X][X][X]     ← don't care
 *
 * Byte 0 RX: [X][X][X][X][X][X][X][X]     ← don't care
 * Byte 1 RX: [X][X][X][X][null][B11][B10][B9] ← null bit then MSBs
 * Byte 2 RX: [B8][B7][B6][B5][B4][B3][B2][B1][B0] (well, B8 is last of byte 1)
 *
 * Result is 12 bits assembled from the received bytes.
 */

spi_err_t mcp3204_init(mcp3204_t *dev, spi_device_t device, uint32_t vref_mv)
{
    if (!dev || !device) return SPI_ERR_INVALID_ARG;
    dev->device      = device;
    dev->vref_mv     = vref_mv ? vref_mv : 3300;
    dev->initialized = true;
    return SPI_OK;
}

spi_err_t mcp3204_read_raw(mcp3204_t *dev, mcp3204_channel_t channel,
                            uint16_t *raw)
{
    if (!dev || !dev->initialized || !raw) return SPI_ERR_INVALID_ARG;

    bool     single = (channel <= MCP3204_CH3);
    uint8_t  ch     = single ? (uint8_t)channel : (uint8_t)(channel - 4);

    /*
     * Build the 3-byte TX sequence:
     * Byte 0: 0x01 (start bit)
     * Byte 1: (SGL << 7) | (ch << 4)
     * Byte 2: 0x00 (don't care)
     */
    uint8_t tx[3] = {
        0x01,
        (uint8_t)((single ? 0x80 : 0x00) | (ch << 4)),
        0x00
    };
    uint8_t rx[3] = {0};

    spi_err_t err;
    err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;

    spi_xfer_t xfer = { .tx_buf = tx, .rx_buf = rx, .len = 3 };
    err = spi_transfer(dev->device, &xfer);
    spi_transaction_end(dev->device);
    if (err != SPI_OK) return err;

    /* Extract 12-bit result from rx[1] and rx[2]:
     * rx[1] bits [3:0] = B11..B8 (upper 4 bits)
     * rx[2] bits [7:0] = B7..B0  (lower 8 bits)
     */
    *raw = (uint16_t)(((rx[1] & 0x0F) << 8) | rx[2]);
    return SPI_OK;
}

spi_err_t mcp3204_read_mv(mcp3204_t *dev, mcp3204_channel_t channel,
                           uint32_t *mv)
{
    if (!dev || !mv) return SPI_ERR_INVALID_ARG;

    uint16_t raw;
    CHECK(mcp3204_read_raw(dev, channel, &raw));

    /* voltage_mv = raw * vref_mv / 4095 */
    *mv = ((uint32_t)raw * dev->vref_mv) / 4095;
    return SPI_OK;
}

spi_err_t mcp3204_read_all(mcp3204_t *dev, uint16_t raw[4])
{
    if (!dev || !raw) return SPI_ERR_INVALID_ARG;

    for (uint8_t ch = 0; ch < 4; ch++) {
        CHECK(mcp3204_read_raw(dev, (mcp3204_channel_t)ch, &raw[ch]));
    }
    return SPI_OK;
}

/* ============================================================================
 * ST7735 IMPLEMENTATION
 * ============================================================================
 *
 * ST7735 COMMUNICATION:
 *   The DC (Data/Command) pin distinguishes commands from pixel data:
 *   DC LOW  → next byte(s) are command bytes
 *   DC HIGH → next byte(s) are data/parameter bytes
 *
 * WINDOW ADDRESSING:
 *   Before writing pixels, set the address window:
 *   1. CASET (0x2A): set column start/end [0..width-1]
 *   2. RASET (0x2B): set row start/end [0..height-1]
 *   3. RAMWR (0x2C): begin pixel write
 *   4. Clock out w×h pixels (2 bytes each in RGB565)
 *
 * INITIALIZATION SEQUENCE:
 *   The ST7735 requires a specific init sequence with precise timing.
 *   Deviating from this sequence causes display artifacts or no output.
 */

/** Send a command byte (DC=LOW) */
static spi_err_t st7735_cmd(st7735_t *dev, uint8_t cmd)
{
    dev->dc_write(false, dev->hal_ctx);     /* DC LOW = command */
    spi_err_t err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;
    spi_xfer_t xfer = { .tx_buf = &cmd, .rx_buf = NULL, .len = 1 };
    err = spi_transfer(dev->device, &xfer);
    spi_transaction_end(dev->device);
    return err;
}

/** Send data bytes (DC=HIGH) */
static spi_err_t st7735_data(st7735_t *dev, const uint8_t *data, size_t len)
{
    dev->dc_write(true, dev->hal_ctx);      /* DC HIGH = data */
    spi_err_t err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;
    spi_xfer_t xfer = { .tx_buf = data, .rx_buf = NULL, .len = len };
    err = spi_transfer(dev->device, &xfer);
    spi_transaction_end(dev->device);
    return err;
}

/** Set the pixel address window before writing pixels */
static spi_err_t st7735_set_window(st7735_t *dev,
                                    uint8_t x0, uint8_t y0,
                                    uint8_t x1, uint8_t y1)
{
    uint8_t col_data[4] = {
        0, (uint8_t)(x0 + dev->col_offset),
        0, (uint8_t)(x1 + dev->col_offset)
    };
    uint8_t row_data[4] = {
        0, (uint8_t)(y0 + dev->row_offset),
        0, (uint8_t)(y1 + dev->row_offset)
    };

    CHECK(st7735_cmd(dev, ST7735_CMD_CASET));
    CHECK(st7735_data(dev, col_data, 4));
    CHECK(st7735_cmd(dev, ST7735_CMD_RASET));
    CHECK(st7735_data(dev, row_data, 4));
    CHECK(st7735_cmd(dev, ST7735_CMD_RAMWR));
    return SPI_OK;
}

/** Millisecond delay helper — uses HAL delay_us */
static void delay_ms_hal(st7735_t *dev, uint32_t ms)
{
    /* Approximate: call delay_us 1000 times for 1ms each */
    /* In real code, replace with proper platform delay */
    (void)dev; (void)ms;
}

spi_err_t st7735_init(st7735_t *dev, spi_device_t device,
                       void (*dc_write)(bool, void *),
                       void (*rst_write)(bool, void *),
                       void *hal_ctx)
{
    if (!dev || !device || !dc_write) return SPI_ERR_INVALID_ARG;

    dev->device    = device;
    dev->dc_write  = dc_write;
    dev->rst_write = rst_write;
    dev->hal_ctx   = hal_ctx;
    dev->width     = ST7735_WIDTH;
    dev->height    = ST7735_HEIGHT;
    dev->col_offset = 2;    /* Common offset for 128×160 modules */
    dev->row_offset = 1;

    /* Hardware reset */
    if (rst_write) {
        rst_write(false, hal_ctx);  /* RST LOW */
        delay_ms_hal(dev, 20);
        rst_write(true, hal_ctx);   /* RST HIGH */
        delay_ms_hal(dev, 150);
    }

    /* Software reset */
    CHECK(st7735_cmd(dev, ST7735_CMD_SWRESET));
    delay_ms_hal(dev, 150);

    /* Sleep out */
    CHECK(st7735_cmd(dev, ST7735_CMD_SLPOUT));
    delay_ms_hal(dev, 120);     /* MANDATORY: must wait 120ms after sleep-out */

    /* Frame rate control: fosc / (1 × 2 + 40) × (LINE + 2C + 2D) */
    CHECK(st7735_cmd(dev, ST7735_CMD_FRMCTR1));
    uint8_t frmctr[] = { 0x01, 0x2C, 0x2D };
    CHECK(st7735_data(dev, frmctr, 3));

    /* Memory data access: portrait mode, RGB order */
    CHECK(st7735_cmd(dev, ST7735_CMD_MADCTL));
    uint8_t madctl = ST7735_MADCTL_RGB;
    CHECK(st7735_data(dev, &madctl, 1));

    /* 16-bit RGB565 color format */
    CHECK(st7735_cmd(dev, ST7735_CMD_COLMOD));
    uint8_t colmod = ST7735_COLMOD_16BIT;
    CHECK(st7735_data(dev, &colmod, 1));

    /* Normal display on */
    CHECK(st7735_cmd(dev, ST7735_CMD_NORON));
    delay_ms_hal(dev, 10);

    /* Display on */
    CHECK(st7735_cmd(dev, ST7735_CMD_DISPON));
    delay_ms_hal(dev, 100);

    dev->initialized = true;
    return SPI_OK;
}

spi_err_t st7735_display_on(st7735_t *dev, bool on)
{
    if (!dev || !dev->initialized) return SPI_ERR_NOT_INIT;
    return st7735_cmd(dev, on ? ST7735_CMD_DISPON : ST7735_CMD_DISPOFF);
}

spi_err_t st7735_fill(st7735_t *dev, uint16_t color)
{
    return st7735_fill_rect(dev, 0, 0, dev->width, dev->height, color);
}

spi_err_t st7735_draw_pixel(st7735_t *dev, uint8_t x, uint8_t y, uint16_t color)
{
    if (!dev || !dev->initialized) return SPI_ERR_NOT_INIT;
    if (x >= dev->width || y >= dev->height) return SPI_ERR_INVALID_ARG;

    CHECK(st7735_set_window(dev, x, y, x, y));
    dev->dc_write(true, dev->hal_ctx);

    uint8_t px[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    spi_err_t err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;
    spi_xfer_t xfer = { .tx_buf = px, .rx_buf = NULL, .len = 2 };
    err = spi_transfer(dev->device, &xfer);
    spi_transaction_end(dev->device);
    return err;
}

spi_err_t st7735_fill_rect(st7735_t *dev, uint8_t x, uint8_t y,
                             uint8_t w, uint8_t h, uint16_t color)
{
    if (!dev || !dev->initialized) return SPI_ERR_NOT_INIT;
    if (x + w > dev->width || y + h > dev->height) return SPI_ERR_INVALID_ARG;

    CHECK(st7735_set_window(dev, x, y, (uint8_t)(x+w-1), (uint8_t)(y+h-1)));

    /* Prepare 2-byte pixel value */
    uint8_t px[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    uint32_t total = (uint32_t)w * h;

    dev->dc_write(true, dev->hal_ctx);
    spi_err_t err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;

    /* Send each pixel — 2 bytes per pixel */
    for (uint32_t i = 0; i < total; i++) {
        spi_xfer_t xfer = { .tx_buf = px, .rx_buf = NULL, .len = 2 };
        err = spi_transfer(dev->device, &xfer);
        if (err != SPI_OK) break;
    }

    spi_transaction_end(dev->device);
    return err;
}

spi_err_t st7735_blit(st7735_t *dev, uint8_t x, uint8_t y,
                       uint8_t w, uint8_t h, const uint16_t *pixels)
{
    if (!dev || !dev->initialized || !pixels) return SPI_ERR_NOT_INIT;
    if (x + w > dev->width || y + h > dev->height) return SPI_ERR_INVALID_ARG;

    CHECK(st7735_set_window(dev, x, y, (uint8_t)(x+w-1), (uint8_t)(y+h-1)));

    uint32_t total = (uint32_t)w * h;
    dev->dc_write(true, dev->hal_ctx);
    spi_err_t err = spi_transaction_begin(dev->device);
    if (err != SPI_OK) return err;

    for (uint32_t i = 0; i < total; i++) {
        uint8_t px[2] = { (uint8_t)(pixels[i] >> 8), (uint8_t)(pixels[i] & 0xFF) };
        spi_xfer_t xfer = { .tx_buf = px, .rx_buf = NULL, .len = 2 };
        err = spi_transfer(dev->device, &xfer);
        if (err != SPI_OK) break;
    }

    spi_transaction_end(dev->device);
    return err;
}

spi_err_t st7735_set_rotation(st7735_t *dev, uint8_t rotation)
{
    if (!dev || !dev->initialized) return SPI_ERR_NOT_INIT;

    uint8_t madctl;
    switch (rotation % 4) {
        case 0:  madctl = ST7735_MADCTL_RGB;                                       break;
        case 1:  madctl = ST7735_MADCTL_MX | ST7735_MADCTL_MV | ST7735_MADCTL_RGB; break;
        case 2:  madctl = ST7735_MADCTL_MX | ST7735_MADCTL_MY | ST7735_MADCTL_RGB; break;
        case 3:  madctl = ST7735_MADCTL_MY | ST7735_MADCTL_MV | ST7735_MADCTL_RGB; break;
        default: madctl = ST7735_MADCTL_RGB;
    }

    CHECK(st7735_cmd(dev, ST7735_CMD_MADCTL));
    return st7735_data(dev, &madctl, 1);
}
