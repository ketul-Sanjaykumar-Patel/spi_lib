/**
 * @file    spi_devices.h
 * @brief   Ready-to-use drivers for common real-world SPI devices
 *
 * COVERED DEVICES:
 *
 *  1. W25Q128 — 128Mbit NOR Flash Memory (Winbond)
 *     Used in: ESP32/ESP8266 modules, STM32 boards, routers, IoT gateways
 *     SPI Mode: 0 or 3, up to 104 MHz
 *
 *  2. MAX31855 — Thermocouple-to-Digital Converter (Maxim/Analog Devices)
 *     Used in: 3D printers (hotend temp), industrial ovens, reflow stations
 *     SPI Mode: 1 (read-only, 32-bit frame)
 *
 *  3. MCP3204 — 4-Channel 12-bit SPI ADC (Microchip)
 *     Used in: Precision analog measurement, audio, sensor digitization,
 *              oscilloscopes, data acquisition systems
 *     SPI Mode: 0, up to 1.8 MHz (5V) or 900 kHz (2.7V)
 *
 *  4. ST7735 — 132×162 Color TFT LCD Controller (Sitronix)
 *     Used in: Small embedded displays, handheld instruments,
 *              IoT dashboards, dev board shields
 *     SPI Mode: 3 (or 0), up to 15 MHz
 */

#ifndef SPI_DEVICES_H
#define SPI_DEVICES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "spi.h"

/* ============================================================================
 * 1. W25Q128 — 128Mbit (16MB) NOR Flash
 *    Datasheet: https://www.winbond.com/resource-files/w25q128jv%20revf%2003272018%20plus.pdf
 *
 *    REAL-WORLD USAGE:
 *    - ESP32 / ESP8266 modules: stores firmware (4MB typically)
 *    - STM32 Discovery boards: external flash for large assets
 *    - Home routers (OpenWrt): stores router firmware
 *    - Industrial PLCs: parameter and recipe storage
 *    - FPGA configuration storage
 *
 *    KEY SPECS:
 *    - 16MB capacity, 256-byte pages, 4KB sectors, 32KB/64KB blocks
 *    - Read: up to 104 MHz
 *    - Page Program (write): ~3ms max
 *    - Sector Erase (4KB): ~400ms max — MUST erase before write
 *    - Chip Erase: ~200 seconds max
 *    - Endurance: 100,000 erase cycles per sector
 *    - Data retention: 20 years
 * ========================================================================== */

#define W25Q128_CMD_WRITE_ENABLE    0x06    /**< Enable write/erase — MUST send before every write */
#define W25Q128_CMD_WRITE_DISABLE   0x04    /**< Disable write */
#define W25Q128_CMD_READ_STATUS1    0x05    /**< Read Status Register 1 */
#define W25Q128_CMD_READ_STATUS2    0x35    /**< Read Status Register 2 */
#define W25Q128_CMD_READ_DATA       0x03    /**< Read data (up to 50 MHz) */
#define W25Q128_CMD_FAST_READ       0x0B    /**< Fast read with dummy byte (104 MHz) */
#define W25Q128_CMD_PAGE_PROGRAM    0x02    /**< Write up to 256 bytes to one page */
#define W25Q128_CMD_SECTOR_ERASE    0x20    /**< Erase 4KB sector */
#define W25Q128_CMD_BLOCK_ERASE_32K 0x52    /**< Erase 32KB block */
#define W25Q128_CMD_BLOCK_ERASE_64K 0xD8    /**< Erase 64KB block */
#define W25Q128_CMD_CHIP_ERASE      0xC7    /**< Erase entire chip */
#define W25Q128_CMD_JEDEC_ID        0x9F    /**< Read manufacturer + device ID */
#define W25Q128_CMD_UNIQUE_ID       0x4B    /**< Read 64-bit unique ID */
#define W25Q128_CMD_POWER_DOWN      0xB9    /**< Enter power-down (~1µA) */
#define W25Q128_CMD_RELEASE_PD      0xAB    /**< Release power-down */

#define W25Q128_STATUS_BUSY         0x01    /**< Status1 bit 0: device is busy */
#define W25Q128_STATUS_WEL          0x02    /**< Status1 bit 1: write enable latch */

#define W25Q128_MFR_ID              0xEF    /**< Winbond manufacturer ID */
#define W25Q128_DEV_ID              0x4018  /**< W25Q128 device ID */

#define W25Q128_PAGE_SIZE           256     /**< Bytes per page */
#define W25Q128_SECTOR_SIZE         4096    /**< Bytes per sector (smallest erasable unit) */
#define W25Q128_BLOCK_SIZE_32K      32768
#define W25Q128_BLOCK_SIZE_64K      65536
#define W25Q128_CAPACITY            (16 * 1024 * 1024)  /**< 16MB total */

#define W25Q128_PROG_TIMEOUT_MS     10      /**< Page program timeout */
#define W25Q128_ERASE_SECTOR_MS     400     /**< Sector erase timeout */
#define W25Q128_ERASE_BLOCK_MS      2000    /**< Block erase timeout */

/** W25Q128 driver handle */
typedef struct {
    spi_device_t device;
    bool         initialized;
} w25q128_t;

/**
 * @brief Initialize W25Q128 Flash
 *
 * Verifies JEDEC ID (0xEF 0x40 0x18), wakes from power-down if needed.
 *
 * @param dev     W25Q128 handle
 * @param device  Registered SPI device handle (speed=40MHz, mode=SPI_MODE_0)
 * @return SPI_OK on success, SPI_ERR_PLATFORM if JEDEC ID mismatch
 *
 * @example
 * @code
 * spi_device_cfg_t cfg = { .cs_id=0, .mode=SPI_MODE_0, .speed_hz=40000000 };
 * spi_register_device(bus, &cfg, &spi_dev);
 * w25q128_t flash;
 * w25q128_init(&flash, spi_dev);
 * @endcode
 */
spi_err_t w25q128_init(w25q128_t *dev, spi_device_t device);

/**
 * @brief Read bytes from flash
 *
 * Uses FAST_READ command (0x0B) with one dummy byte for maximum speed.
 *
 * @param dev      W25Q128 handle
 * @param addr     24-bit byte address (0x000000 – 0xFFFFFF)
 * @param data     Buffer to store read data
 * @param len      Number of bytes to read
 * @return SPI_OK on success
 */
spi_err_t w25q128_read(w25q128_t *dev, uint32_t addr, uint8_t *data, size_t len);

/**
 * @brief Write bytes to flash (handles page boundary splits automatically)
 *
 * IMPORTANT: The target address range MUST be erased before writing.
 *            Flash can only change bits from 1→0, never 0→1.
 *            Use w25q128_erase_sector() first.
 *
 * Automatically splits writes across 256-byte page boundaries.
 * Each page program waits for completion (polls BUSY bit).
 *
 * @param dev      W25Q128 handle
 * @param addr     Start address (any alignment)
 * @param data     Data to write
 * @param len      Number of bytes
 * @return SPI_OK on success
 */
spi_err_t w25q128_write(w25q128_t *dev, uint32_t addr,
                         const uint8_t *data, size_t len);

/**
 * @brief Erase a 4KB sector
 *
 * Erases all bytes in the sector containing 'addr' to 0xFF.
 * Takes up to 400ms. Blocks until complete.
 *
 * @param dev   W25Q128 handle
 * @param addr  Any address within the sector to erase
 * @return SPI_OK on success
 */
spi_err_t w25q128_erase_sector(w25q128_t *dev, uint32_t addr);

/**
 * @brief Erase entire chip (all 16MB → 0xFF)
 *
 * Takes up to 200 seconds. Use with care!
 *
 * @param dev  W25Q128 handle
 * @return SPI_OK on success
 */
spi_err_t w25q128_erase_chip(w25q128_t *dev);

/**
 * @brief Read JEDEC manufacturer + device ID
 *
 * @param dev          W25Q128 handle
 * @param[out] mfr_id  Manufacturer ID (0xEF for Winbond)
 * @param[out] dev_id  Device ID (0x4018 for W25Q128)
 * @return SPI_OK on success
 */
spi_err_t w25q128_read_id(w25q128_t *dev, uint8_t *mfr_id, uint16_t *dev_id);

/**
 * @brief Check if device is busy (write/erase in progress)
 *
 * @param dev  W25Q128 handle
 * @return true if busy, false if idle
 */
bool w25q128_is_busy(w25q128_t *dev);

/**
 * @brief Enter ultra-low-power mode (~1µA current)
 *
 * Call w25q128_wake() before next access.
 */
spi_err_t w25q128_sleep(w25q128_t *dev);

/**
 * @brief Wake from power-down mode
 */
spi_err_t w25q128_wake(w25q128_t *dev);

/* ============================================================================
 * 2. MAX31855 — Cold-Junction Compensated Thermocouple Amplifier
 *    Datasheet: https://datasheets.maximintegrated.com/en/ds/MAX31855.pdf
 *
 *    REAL-WORLD USAGE:
 *    - 3D printers: hotend temperature (200–300°C range)
 *    - Reflow ovens: solder paste temperature profile
 *    - Industrial kilns and furnaces
 *    - BBQ / smoker temperature controllers
 *    - HVAC duct temperature sensing
 *
 *    KEY SPECS:
 *    - Thermocouple input: -200°C to +1350°C (Type K)
 *    - Cold junction compensation: -40°C to +125°C
 *    - Resolution: 0.25°C (thermocouple), 0.0625°C (junction)
 *    - SPI: READ ONLY — 32-bit frame, Mode 1, up to 5 MHz
 *    - No MOSI needed — just clock out 32 bits and read MISO
 *    - Fault detection: open circuit, short to VCC, short to GND
 * ========================================================================== */

/** MAX31855 fault bit masks (in the 32-bit data frame) */
#define MAX31855_FAULT_OC      (1 << 0)   /**< Open circuit — no thermocouple connected */
#define MAX31855_FAULT_SCG     (1 << 1)   /**< Short to GND */
#define MAX31855_FAULT_SCV     (1 << 2)   /**< Short to VCC */
#define MAX31855_FAULT_ANY     (1 << 16)  /**< Fault bit (any of the above) */

/**
 * @brief MAX31855 reading result
 */
typedef struct {
    int32_t thermocouple_c4;  /**< Thermocouple temp × 4 (divide by 4 for °C) */
    int32_t junction_c16;     /**< Cold junction temp × 16 (divide by 16 for °C) */
    uint8_t fault;            /**< Fault bits: MAX31855_FAULT_OC/SCG/SCV */
    bool    valid;            /**< true if no faults detected */
} max31855_reading_t;

/** MAX31855 driver handle */
typedef struct {
    spi_device_t device;
    bool         initialized;
} max31855_t;

/**
 * @brief Initialize MAX31855 thermocouple amplifier
 *
 * No configuration needed — device is purely read-only.
 * Recommended device config: mode=SPI_MODE_1, speed=4000000 (4 MHz)
 *
 * @param dev     MAX31855 handle
 * @param device  Registered SPI device handle
 * @return SPI_OK always
 */
spi_err_t max31855_init(max31855_t *dev, spi_device_t device);

/**
 * @brief Read thermocouple and cold junction temperatures
 *
 * Clocks out 32 bits from the device (MOSI ignored):
 *   Bits [31:18] = Thermocouple temperature (14-bit signed, 0.25°C/LSB)
 *   Bit  [16]    = Fault flag
 *   Bits [15:4]  = Junction temperature (12-bit signed, 0.0625°C/LSB)
 *   Bits [2:0]   = Fault type (OC, SCG, SCV)
 *
 * @param dev      MAX31855 handle
 * @param reading  Pointer to result struct
 * @return SPI_OK on success, SPI_ERR_PLATFORM on fault
 *
 * @example
 * @code
 * max31855_reading_t r;
 * max31855_read(&tc, &r);
 * if (r.valid) {
 *     printf("Temp: %d.%02d C\n", r.thermocouple_c4/4, (r.thermocouple_c4%4)*25);
 * } else {
 *     printf("Fault: %s\n",
 *         r.fault & MAX31855_FAULT_OC  ? "open circuit" :
 *         r.fault & MAX31855_FAULT_SCG ? "short to GND" : "short to VCC");
 * }
 * @endcode
 */
spi_err_t max31855_read(max31855_t *dev, max31855_reading_t *reading);

/**
 * @brief Read thermocouple temperature in hundredths of °C
 *
 * Convenience function — returns temperature as integer (e.g. 24750 = 247.50°C).
 * Returns INT32_MIN on fault.
 *
 * @param dev  MAX31855 handle
 * @return temperature × 100 in °C, or INT32_MIN on error
 */
int32_t max31855_read_temp_c100(max31855_t *dev);

/* ============================================================================
 * 3. MCP3204 — 4-Channel 12-bit SPI ADC
 *    Datasheet: https://ww1.microchip.com/downloads/en/DeviceDoc/21298e.pdf
 *
 *    REAL-WORLD USAGE:
 *    - Precision analog input for microcontrollers with no/poor ADC
 *    - Audio digitization (low-speed audio sampling)
 *    - Sensor digitization (load cells, pressure sensors via Wheatstone bridge)
 *    - Industrial 4-20mA current loop measurement (with input resistor)
 *    - Raspberry Pi analog input (Pi has no built-in ADC)
 *    - Battery voltage monitoring
 *
 *    KEY SPECS:
 *    - 4 single-ended or 2 differential input channels
 *    - 12-bit resolution (0–4095 counts)
 *    - VDD reference: 0 to VDD (ratiometric)
 *    - External VREF supported
 *    - Max sampling rate: 100ksps @ 5V
 *    - SPI Mode: 0 or 1, up to 1.8 MHz (5V), 900 kHz (2.7V)
 * ========================================================================== */

/** MCP3204 channel selection */
typedef enum {
    MCP3204_CH0 = 0,    /**< Single-ended channel 0 */
    MCP3204_CH1 = 1,    /**< Single-ended channel 1 */
    MCP3204_CH2 = 2,    /**< Single-ended channel 2 */
    MCP3204_CH3 = 3,    /**< Single-ended channel 3 */
    MCP3204_DIFF_0_1 = 4, /**< Differential: CH0(+) CH1(-) */
    MCP3204_DIFF_1_0 = 5, /**< Differential: CH1(+) CH0(-) */
    MCP3204_DIFF_2_3 = 6, /**< Differential: CH2(+) CH3(-) */
    MCP3204_DIFF_3_2 = 7, /**< Differential: CH3(+) CH2(-) */
} mcp3204_channel_t;

/** MCP3204 driver handle */
typedef struct {
    spi_device_t device;
    uint32_t     vref_mv;    /**< Reference voltage in millivolts (default 3300) */
    bool         initialized;
} mcp3204_t;

/**
 * @brief Initialize MCP3204 ADC
 *
 * Recommended device config: mode=SPI_MODE_0, speed=1000000 (1 MHz)
 *
 * @param dev      MCP3204 handle
 * @param device   Registered SPI device handle
 * @param vref_mv  Reference voltage in millivolts (e.g. 3300 for 3.3V)
 * @return SPI_OK on success
 */
spi_err_t mcp3204_init(mcp3204_t *dev, spi_device_t device, uint32_t vref_mv);

/**
 * @brief Read raw ADC value (0–4095)
 *
 * MCP3204 communication protocol (3-byte SPI transaction):
 *
 *   TX: [start|SGL/DIFF|D2] [D1|D0|X|X|X|X|X|X] [X|X|X|X|X|X|X|X]
 *   RX: [X|X|X|X|X|X|X|X]  [X|X|X|null|B11|B10|B9|B8] [B7|B6|B5|B4|B3|B2|B1|B0]
 *
 * @param dev      MCP3204 handle
 * @param channel  Channel to sample
 * @param[out] raw Raw 12-bit ADC result (0–4095)
 * @return SPI_OK on success
 *
 * @example
 * @code
 * uint16_t raw;
 * mcp3204_read_raw(&adc, MCP3204_CH0, &raw);
 * printf("Raw: %u / 4095\n", raw);
 * @endcode
 */
spi_err_t mcp3204_read_raw(mcp3204_t *dev, mcp3204_channel_t channel,
                            uint16_t *raw);

/**
 * @brief Read ADC value converted to millivolts
 *
 * Applies: voltage_mv = (raw * vref_mv) / 4095
 *
 * @param dev        MCP3204 handle
 * @param channel    Channel to sample
 * @param[out] mv    Voltage in millivolts
 * @return SPI_OK on success
 *
 * @example
 * @code
 * uint32_t mv;
 * mcp3204_read_mv(&adc, MCP3204_CH0, &mv);
 * printf("Voltage: %u.%03u V\n", mv/1000, mv%1000);
 * @endcode
 */
spi_err_t mcp3204_read_mv(mcp3204_t *dev, mcp3204_channel_t channel,
                           uint32_t *mv);

/**
 * @brief Read all 4 single-ended channels in sequence
 *
 * @param dev      MCP3204 handle
 * @param[out] raw Array of 4 raw values [CH0, CH1, CH2, CH3]
 * @return SPI_OK on success
 */
spi_err_t mcp3204_read_all(mcp3204_t *dev, uint16_t raw[4]);

/* ============================================================================
 * 4. ST7735 — 132×162 Color TFT LCD Controller
 *    Datasheet: https://www.displayfuture.com/Display/datasheet/controller/ST7735.pdf
 *
 *    REAL-WORLD USAGE:
 *    - Arduino TFT shields (Adafruit 1.8" TFT, many clones)
 *    - ESP32/STM32 project displays
 *    - Handheld instrument displays
 *    - IoT dashboards
 *    - Wearable device displays
 *
 *    KEY SPECS:
 *    - Resolution: 128×160 (typical) or 132×162 (full)
 *    - Color depth: 12-bit (4096 colors) or 16-bit (65536 colors)
 *    - SPI: Mode 3 (CPOL=1, CPHA=1), up to 15 MHz
 *    - Extra pin: DC (Data/Command) — HIGH=data, LOW=command
 *    - Extra pin: RST (Reset, active LOW)
 *    - Extra pin: BL (Backlight, PWM control)
 *
 *    IMPORTANT: ST7735 uses a DC pin to distinguish command bytes from
 *    data bytes — this is NOT part of SPI, it's a separate GPIO.
 *    The HAL must support setting this pin.
 * ========================================================================== */

/** ST7735 command set (partial) */
#define ST7735_CMD_NOP          0x00
#define ST7735_CMD_SWRESET      0x01    /**< Software reset */
#define ST7735_CMD_SLPIN        0x10    /**< Enter sleep mode */
#define ST7735_CMD_SLPOUT       0x11    /**< Exit sleep mode (wait 120ms) */
#define ST7735_CMD_NORON        0x13    /**< Normal display mode on */
#define ST7735_CMD_INVOFF       0x20    /**< Display inversion off */
#define ST7735_CMD_INVON        0x21    /**< Display inversion on */
#define ST7735_CMD_GAMSET       0x26    /**< Gamma set */
#define ST7735_CMD_DISPOFF      0x28    /**< Display off */
#define ST7735_CMD_DISPON       0x29    /**< Display on */
#define ST7735_CMD_CASET        0x2A    /**< Column address set */
#define ST7735_CMD_RASET        0x2B    /**< Row address set */
#define ST7735_CMD_RAMWR        0x2C    /**< Write to frame RAM */
#define ST7735_CMD_MADCTL       0x36    /**< Memory data access control (rotation) */
#define ST7735_CMD_COLMOD       0x3A    /**< Interface pixel format */
#define ST7735_CMD_FRMCTR1      0xB1    /**< Frame rate control (normal mode) */
#define ST7735_CMD_PWCTR1       0xC0    /**< Power control 1 */
#define ST7735_CMD_VMCTR1       0xC5    /**< VCOM control 1 */
#define ST7735_CMD_GMCTRP1      0xE0    /**< Positive gamma correction */
#define ST7735_CMD_GMCTRN1      0xE1    /**< Negative gamma correction */

/** ST7735 pixel format */
#define ST7735_COLMOD_12BIT     0x03
#define ST7735_COLMOD_16BIT     0x05    /**< RGB565 — most common */

/** ST7735 MADCTL rotation bits */
#define ST7735_MADCTL_MY        0x80    /**< Row address order */
#define ST7735_MADCTL_MX        0x40    /**< Column address order */
#define ST7735_MADCTL_MV        0x20    /**< Row/column exchange */
#define ST7735_MADCTL_RGB       0x00    /**< RGB order */
#define ST7735_MADCTL_BGR       0x08    /**< BGR order */

/** Common 16-bit RGB565 colors */
#define ST7735_BLACK    0x0000
#define ST7735_WHITE    0xFFFF
#define ST7735_RED      0xF800
#define ST7735_GREEN    0x07E0
#define ST7735_BLUE     0x001F
#define ST7735_CYAN     0x07FF
#define ST7735_MAGENTA  0xF81F
#define ST7735_YELLOW   0xFFE0

/** Display dimensions */
#define ST7735_WIDTH    128
#define ST7735_HEIGHT   160

/**
 * @brief ST7735 driver handle
 *
 * The DC (Data/Command) pin is controlled via a separate HAL callback
 * because it is not part of the SPI protocol.
 */
typedef struct {
    spi_device_t device;
    uint8_t      width;
    uint8_t      height;
    uint8_t      col_offset;    /**< Column offset (varies by module) */
    uint8_t      row_offset;    /**< Row offset (varies by module) */
    /** Set DC pin: true=data, false=command */
    void         (*dc_write)(bool data, void *ctx);
    /** Set RST pin: true=high (normal), false=low (reset) */
    void         (*rst_write)(bool level, void *ctx);
    void         *hal_ctx;
    bool          initialized;
} st7735_t;

/**
 * @brief Initialize ST7735 TFT display
 *
 * Performs hardware reset, sends full initialization sequence,
 * sets 16-bit RGB565 color mode.
 *
 * Recommended device config: mode=SPI_MODE_3, speed=15000000 (15 MHz)
 *
 * @param dev        ST7735 handle
 * @param device     Registered SPI device handle
 * @param dc_write   Callback to set DC pin (HIGH=data, LOW=command)
 * @param rst_write  Callback to set RST pin (LOW to reset)
 * @param hal_ctx    Context pointer passed to dc_write and rst_write
 * @return SPI_OK on success
 */
spi_err_t st7735_init(st7735_t *dev, spi_device_t device,
                       void (*dc_write)(bool, void *),
                       void (*rst_write)(bool, void *),
                       void *hal_ctx);

/**
 * @brief Fill entire screen with one color
 *
 * @param dev    ST7735 handle
 * @param color  16-bit RGB565 color
 * @return SPI_OK on success
 */
spi_err_t st7735_fill(st7735_t *dev, uint16_t color);

/**
 * @brief Draw a single pixel
 *
 * @param dev    ST7735 handle
 * @param x      Column (0 to width-1)
 * @param y      Row (0 to height-1)
 * @param color  16-bit RGB565 color
 * @return SPI_OK on success
 */
spi_err_t st7735_draw_pixel(st7735_t *dev, uint8_t x, uint8_t y, uint16_t color);

/**
 * @brief Fill a rectangle with a solid color
 *
 * Sets column + row address window, then bulk-writes color pixels.
 * This is the most efficient way to draw filled areas.
 *
 * @param dev    ST7735 handle
 * @param x      Left edge
 * @param y      Top edge
 * @param w      Width in pixels
 * @param h      Height in pixels
 * @param color  16-bit RGB565 color
 * @return SPI_OK on success
 */
spi_err_t st7735_fill_rect(st7735_t *dev, uint8_t x, uint8_t y,
                             uint8_t w, uint8_t h, uint16_t color);

/**
 * @brief Write a raw pixel buffer to a rectangle
 *
 * Most efficient for bitmaps and images.
 * Buffer must contain w×h pixels in RGB565 format (big-endian).
 *
 * @param dev     ST7735 handle
 * @param x       Left edge
 * @param y       Top edge
 * @param w       Width in pixels
 * @param h       Height in pixels
 * @param pixels  RGB565 pixel data (2 bytes per pixel, w×h pixels total)
 * @return SPI_OK on success
 */
spi_err_t st7735_blit(st7735_t *dev, uint8_t x, uint8_t y,
                       uint8_t w, uint8_t h, const uint16_t *pixels);

/**
 * @brief Set display rotation
 *
 * @param dev       ST7735 handle
 * @param rotation  0=portrait, 1=landscape, 2=portrait inverted, 3=landscape inverted
 * @return SPI_OK on success
 */
spi_err_t st7735_set_rotation(st7735_t *dev, uint8_t rotation);

/**
 * @brief Turn display on or off
 */
spi_err_t st7735_display_on(st7735_t *dev, bool on);

/**
 * @brief Convert RGB888 to RGB565
 *
 * @param r  Red   (0–255)
 * @param g  Green (0–255)
 * @param b  Blue  (0–255)
 * @return   16-bit RGB565 color
 */
static inline uint16_t st7735_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) |
           ((uint16_t)(g & 0xFC) << 3) |
           ((uint16_t)(b >> 3));
}

#ifdef __cplusplus
}
#endif

#endif /* SPI_DEVICES_H */
