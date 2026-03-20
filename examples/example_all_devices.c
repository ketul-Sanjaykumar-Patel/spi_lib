/**
 * @file    example_all_devices.c
 * @brief   Complete SPI library demo — all 4 device drivers on one bus
 *
 * HARDWARE WIRING (example):
 *
 *   MCU Pin    →  Function
 *   ─────────────────────────────
 *   MOSI (PA7) →  W25Q128 DI, MCP3204 DIN, ST7735 SDA (shared)
 *   MISO (PA6) →  W25Q128 DO, MCP3204 DOUT, MAX31855 SO (shared)
 *   SCK  (PA5) →  All SCK pins (shared)
 *   PA4        →  W25Q128 CS
 *   PA3        →  MAX31855 CS
 *   PA2        →  MCP3204 CS
 *   PA1        →  ST7735 CS
 *   PA0        →  ST7735 DC (Data/Command — NOT SPI, separate GPIO)
 *   PB0        →  ST7735 RST
 *
 * All devices share MOSI/MISO/SCK. CS pins are separate per device.
 * Only the selected device (CS LOW) responds on the bus.
 */

#include <stdio.h>
#include <string.h>
#include "spi.h"
#include "spi_devices.h"

/* ============================================================================
 * PLATFORM HAL — replace with your MCU's GPIO calls
 * ========================================================================== */

/* Simulated GPIO state */
static bool s_mosi = false;
static bool s_miso = false;
static bool s_sck  = false;
static bool s_cs[4] = { true, true, true, true }; /* All CS HIGH (deasserted) */
static bool s_dc  = true;
static bool s_rst = true;

static void platform_mosi_write(bool level, void *ctx)
{
    (void)ctx; s_mosi = level;
    /* STM32: HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, level) */
    /* ESP32: gpio_set_level(GPIO_NUM_23, level)          */
}

static bool platform_miso_read(void *ctx)
{
    (void)ctx; return s_miso;
    /* STM32: return HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == GPIO_PIN_SET */
}

static void platform_sck_write(bool level, void *ctx)
{
    (void)ctx; s_sck = level;
}

/**
 * @brief CS write — routes to correct pin based on cs_id
 *
 * cs_id is set per-device when calling spi_register_device().
 * This callback maps cs_id → the correct GPIO pin.
 */
static void platform_cs_write(uint8_t cs_id, bool level, void *ctx)
{
    (void)ctx;
    if (cs_id < 4) s_cs[cs_id] = level;
    /*
     * STM32 example:
     * switch (cs_id) {
     *   case 0: HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, level); break; // Flash
     *   case 1: HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, level); break; // Thermocouple
     *   case 2: HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, level); break; // ADC
     *   case 3: HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, level); break; // Display
     * }
     */
}

static void platform_delay_us(uint32_t us)
{
    volatile uint32_t n = us * 8;
    while (n--) {}
    /* STM32: DWT_Delay_us(us) or TIM-based */
    /* ESP32: esp_rom_delay_us(us)           */
}

static uint32_t platform_millis(void)
{
    static uint32_t t = 0; return t++;
    /* STM32: return HAL_GetTick() */
    /* ESP32: return (uint32_t)(esp_timer_get_time() / 1000) */
}

/* ST7735 extra pins (not part of SPI) */
static void platform_dc_write(bool level, void *ctx)
{
    (void)ctx; s_dc = level;
    /* HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, level) */
}

static void platform_rst_write(bool level, void *ctx)
{
    (void)ctx; s_rst = level;
    /* HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, level) */
}

static spi_hal_t hal = {
    .mosi_write = platform_mosi_write,
    .miso_read  = platform_miso_read,
    .sck_write  = platform_sck_write,
    .cs_write   = platform_cs_write,
    .delay_us   = platform_delay_us,
    .millis     = platform_millis,
    .user_ctx   = NULL,
};

/* ============================================================================
 * HELPER
 * ========================================================================== */

static bool check(const char *op, spi_err_t err)
{
    if (err != SPI_OK) {
        printf("  ERROR [%s]: %s\n", op, spi_err_to_string(err));
        return false;
    }
    return true;
}

/* ============================================================================
 * EXAMPLE 1: W25Q128 — NOR Flash Read/Write/Erase
 * ============================================================================
 * Real-world pattern: store firmware update, configuration, or log data
 * ========================================================================== */

static void example_w25q128(spi_bus_t bus)
{
    printf("\n=== W25Q128: 16MB NOR Flash ===\n");

    /* Register device: cs_id=0, Mode 0, 40 MHz */
    spi_device_t spi_dev;
    spi_device_cfg_t dev_cfg = {
        .cs_id       = 0,
        .mode        = SPI_MODE_0,
        .speed_hz    = 40000000,
        .bit_order   = SPI_BITORDER_MSB_FIRST,
        .cs_pol      = SPI_CS_ACTIVE_LOW,
        .cs_setup_ns = 100,
        .cs_hold_ns  = 100,
    };
    if (!check("register", spi_register_device(bus, &dev_cfg, &spi_dev))) return;

    w25q128_t flash;
    if (!check("init", w25q128_init(&flash, spi_dev))) return;

    /* Read JEDEC ID */
    uint8_t  mfr;
    uint16_t did;
    check("read_id", w25q128_read_id(&flash, &mfr, &did));
    printf("  JEDEC ID: mfr=0x%02X, device=0x%04X\n", mfr, did);
    printf("  Chip: %s\n", (mfr == 0xEF && did == 0x4018) ? "W25Q128 (Winbond) OK" : "Unknown");

    /* Check busy */
    printf("  Busy: %s\n", w25q128_is_busy(&flash) ? "yes" : "no");

    /* Erase sector 0 (address 0x000000) */
    printf("  Erasing sector 0 (4KB at 0x000000)...\n");
    check("erase_sector", w25q128_erase_sector(&flash, 0x000000));

    /* Write 16 bytes at address 0x000000 */
    uint8_t write_data[16] = {
        0xDE, 0xAD, 0xBE, 0xEF,
        0x01, 0x02, 0x03, 0x04,
        0xCA, 0xFE, 0xBA, 0xBE,
        0x00, 0x11, 0x22, 0x33
    };
    printf("  Writing 16 bytes at 0x000000...\n");
    check("write", w25q128_write(&flash, 0x000000, write_data, 16));

    /* Read back */
    uint8_t read_data[16] = {0};
    check("read", w25q128_read(&flash, 0x000000, read_data, 16));

    bool match = (memcmp(write_data, read_data, 16) == 0);
    printf("  Verify: %s\n", match ? "PASS — data matches" : "FAIL — mismatch");
    printf("  First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n",
           read_data[0], read_data[1], read_data[2], read_data[3]);

    /* Power saving */
    check("sleep", w25q128_sleep(&flash));
    printf("  Flash in power-down mode (~1µA)\n");
    check("wake", w25q128_wake(&flash));
}

/* ============================================================================
 * EXAMPLE 2: MAX31855 — Thermocouple Temperature
 * ============================================================================
 * Real-world pattern: 3D printer hotend PID control
 * ========================================================================== */

static void example_max31855(spi_bus_t bus)
{
    printf("\n=== MAX31855: Thermocouple Amplifier ===\n");

    spi_device_t spi_dev;
    spi_device_cfg_t dev_cfg = {
        .cs_id       = 1,
        .mode        = SPI_MODE_1,      /* MAX31855 requires Mode 1 */
        .speed_hz    = 4000000,         /* 4 MHz max */
        .bit_order   = SPI_BITORDER_MSB_FIRST,
        .cs_pol      = SPI_CS_ACTIVE_LOW,
        .cs_setup_ns = 100,
    };
    if (!check("register", spi_register_device(bus, &dev_cfg, &spi_dev))) return;

    max31855_t tc;
    check("init", max31855_init(&tc, spi_dev));

    max31855_reading_t reading;
    check("read", max31855_read(&tc, &reading));

    if (reading.valid) {
        int32_t tc_whole = reading.thermocouple_c4 / 4;
        int32_t tc_frac  = (reading.thermocouple_c4 % 4) * 25;
        int32_t cj_whole = reading.junction_c16 / 16;

        printf("  Thermocouple : %d.%02d °C\n", tc_whole, tc_frac < 0 ? -tc_frac : tc_frac);
        printf("  Cold junction: %d °C (board temp)\n", cj_whole);
        printf("  Status       : OK — no faults\n");
    } else {
        printf("  FAULT detected:\n");
        if (reading.fault & MAX31855_FAULT_OC)  printf("    - Open circuit (no thermocouple connected)\n");
        if (reading.fault & MAX31855_FAULT_SCG) printf("    - Short to GND\n");
        if (reading.fault & MAX31855_FAULT_SCV) printf("    - Short to VCC\n");
    }

    /* Convenience function */
    int32_t temp_c100 = max31855_read_temp_c100(&tc);
    if (temp_c100 != INT32_MIN) {
        printf("  Temp (c100)  : %d.%02d °C\n", temp_c100 / 100, temp_c100 % 100);
    }
}

/* ============================================================================
 * EXAMPLE 3: MCP3204 — 12-bit ADC
 * ============================================================================
 * Real-world pattern: read potentiometer, pressure sensor, battery voltage
 * ========================================================================== */

static void example_mcp3204(spi_bus_t bus)
{
    printf("\n=== MCP3204: 12-bit ADC (4-channel) ===\n");

    spi_device_t spi_dev;
    spi_device_cfg_t dev_cfg = {
        .cs_id     = 2,
        .mode      = SPI_MODE_0,
        .speed_hz  = 1000000,           /* 1 MHz (safe for 3.3V) */
        .bit_order = SPI_BITORDER_MSB_FIRST,
        .cs_pol    = SPI_CS_ACTIVE_LOW,
    };
    if (!check("register", spi_register_device(bus, &dev_cfg, &spi_dev))) return;

    mcp3204_t adc;
    check("init", mcp3204_init(&adc, spi_dev, 3300)); /* 3.3V reference */

    /* Read single channel */
    uint16_t raw;
    uint32_t mv;
    check("read_raw", mcp3204_read_raw(&adc, MCP3204_CH0, &raw));
    check("read_mv",  mcp3204_read_mv(&adc, MCP3204_CH0, &mv));
    printf("  CH0: raw=%u / 4095,  voltage=%u.%03u V\n", raw, mv/1000, mv%1000);

    /* Read all 4 channels */
    uint16_t all[4];
    check("read_all", mcp3204_read_all(&adc, all));
    for (int i = 0; i < 4; i++) {
        uint32_t ch_mv = ((uint32_t)all[i] * 3300) / 4095;
        printf("  CH%d: raw=%-4u  %u.%03u V\n", i, all[i], ch_mv/1000, ch_mv%1000);
    }

    /* Differential reading CH0(+) vs CH1(-) */
    uint16_t diff_raw;
    check("diff", mcp3204_read_raw(&adc, MCP3204_DIFF_0_1, &diff_raw));
    printf("  CH0-CH1 differential: raw=%u\n", diff_raw);
}

/* ============================================================================
 * EXAMPLE 4: ST7735 — TFT Color Display
 * ============================================================================
 * Real-world pattern: show sensor readings on a small display
 * ========================================================================== */

static void example_st7735(spi_bus_t bus)
{
    printf("\n=== ST7735: 128×160 Color TFT Display ===\n");

    spi_device_t spi_dev;
    spi_device_cfg_t dev_cfg = {
        .cs_id     = 3,
        .mode      = SPI_MODE_3,        /* ST7735 requires Mode 3 */
        .speed_hz  = 15000000,          /* 15 MHz */
        .bit_order = SPI_BITORDER_MSB_FIRST,
        .cs_pol    = SPI_CS_ACTIVE_LOW,
    };
    if (!check("register", spi_register_device(bus, &dev_cfg, &spi_dev))) return;

    st7735_t display;
    check("init", st7735_init(&display, spi_dev,
                               platform_dc_write,
                               platform_rst_write,
                               NULL));

    /* Clear screen to black */
    check("fill black", st7735_fill(&display, ST7735_BLACK));
    printf("  Screen cleared to black\n");

    /* Draw colored rectangles — simulate a dashboard */
    check("fill red bar",   st7735_fill_rect(&display, 0, 0, 128, 40, ST7735_RED));
    check("fill green area",st7735_fill_rect(&display, 0, 40, 128, 80, ST7735_GREEN));
    check("fill blue area", st7735_fill_rect(&display, 0, 120, 128, 40, ST7735_BLUE));
    printf("  Drew dashboard layout (red/green/blue bands)\n");

    /* Single pixel */
    check("pixel", st7735_draw_pixel(&display, 64, 80, ST7735_WHITE));
    printf("  White pixel at center (64, 80)\n");

    /* Custom color using RGB helper */
    uint16_t orange = st7735_rgb(255, 128, 0);
    check("orange rect", st7735_fill_rect(&display, 50, 50, 28, 20, orange));
    printf("  Orange rectangle at (50,50)\n");

    /* Rotation */
    check("rotate landscape", st7735_set_rotation(&display, 1));
    printf("  Rotated to landscape\n");
    check("rotate portrait",  st7735_set_rotation(&display, 0));
    printf("  Rotated back to portrait\n");

    check("display off", st7735_display_on(&display, false));
    check("display on",  st7735_display_on(&display, true));
    printf("  Display on/off cycle complete\n");
}

/* ============================================================================
 * MAIN
 * ========================================================================== */

int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Universal SPI Library v%s — Full Demo      ║\n", spi_version());
    printf("╚══════════════════════════════════════════════╝\n");

    /* Initialize SPI bus (software bit-bang) */
    spi_config_t cfg = {
        .driver     = SPI_MODE_SOFTWARE,
        .timeout_ms = 100,
        .hal        = &hal,
    };

    spi_bus_t bus;
    spi_err_t err = spi_init(&bus, &cfg);
    if (err != SPI_OK) {
        printf("Fatal: spi_init failed: %s\n", spi_err_to_string(err));
        return 1;
    }
    printf("\nSPI bus initialized (software bit-bang mode)\n");
    printf("4 devices will be registered with different speeds/modes\n");

    /* Run all examples */
    example_w25q128(bus);    /* CS0: 40MHz, Mode 0 */
    example_max31855(bus);   /* CS1:  4MHz, Mode 1 */
    example_mcp3204(bus);    /* CS2:  1MHz, Mode 0 */
    example_st7735(bus);     /* CS3: 15MHz, Mode 3 */

    /* Statistics */
    printf("\n=== SPI Bus Statistics ===\n");
    spi_stats_t stats;
    spi_get_stats(bus, &stats);
    printf("  Transfers   : %u\n", stats.transfers);
    printf("  TX bytes    : %u\n", stats.tx_bytes);
    printf("  RX bytes    : %u\n", stats.rx_bytes);
    printf("  CS toggles  : %u\n", stats.cs_toggles);
    printf("  Errors      : %u\n", stats.errors);

    /* Cleanup */
    spi_deinit(bus);
    printf("\nDone. Bus deinitialized.\n");
    return 0;
}
