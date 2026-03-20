# spi_lib — Universal SPI Driver Library for Embedded C

[![Build](https://github.com/ketul-Sanjaykumar-Patel/spi_lib/actions/workflows/build.yml/badge.svg)](https://github.com/ketul-Sanjaykumar-Patel/spi_lib/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-1.0.0-blue.svg)](CHANGELOG.md)
[![C Standard](https://img.shields.io/badge/C-C11-orange.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))

A portable, production-grade SPI master driver for bare-metal and RTOS embedded targets. Supports all 4 clock modes, per-device configuration, and full-duplex transfers via a simple HAL callback interface.

---

## Features

- **Software (bit-bang) SPI** — works on any MCU with 4 GPIO pins
- **All 4 clock modes** (CPOL/CPHA 0–3) implemented correctly
- **Per-device configuration** — each device stores its own speed, mode, CS pin
- **Explicit transaction API** — `spi_transaction_begin()` / `spi_transaction_end()`
- **Typed transfers** — `spi_transfer8()`, `spi_transfer16()`, `spi_transfer32()`
- **Command helpers** — `spi_write_cmd()`, `spi_read_cmd()` for flash/display pattern
- **Bus diagnostics** — statistics counters, error tracking
- **Zero dynamic allocation** — static pool, no heap, RTOS-friendly
- **4 production device drivers** included

---

## Supported Devices

| Device | Category | SPI Mode | Speed | Real-world use |
|--------|----------|----------|-------|----------------|
| W25Q128 | 128Mbit NOR Flash | 0 | 40 MHz | ESP32 firmware, routers, IoT |
| MAX31855 | Thermocouple amplifier | 1 | 4 MHz | 3D printers, industrial ovens |
| MCP3204 | 12-bit 4ch ADC | 0 | 1 MHz | Raspberry Pi analog input |
| ST7735 | 128×160 TFT display | 3 | 15 MHz | Embedded dashboards, instruments |

---

## Quick Start

### 1 — Implement the HAL callbacks

```c
#include "spi.h"

static void my_mosi_write(bool level, void *ctx) { /* set MOSI pin */ }
static bool my_miso_read (void *ctx)             { /* read MISO pin */ return false; }
static void my_sck_write (bool level, void *ctx) { /* set SCK pin  */ }
static void my_cs_write  (uint8_t cs_id, bool level, void *ctx) { /* set CS pin */ }
static void my_delay_us  (uint32_t us)           { /* microsecond delay */ }
static uint32_t my_millis(void)                  { /* millisecond tick  */ return 0; }

static spi_hal_t hal = {
    .mosi_write = my_mosi_write,
    .miso_read  = my_miso_read,
    .sck_write  = my_sck_write,
    .cs_write   = my_cs_write,
    .delay_us   = my_delay_us,
    .millis     = my_millis,
};
```

### 2 — Initialize the bus

```c
spi_bus_t bus;
spi_config_t cfg = {
    .driver     = SPI_MODE_SOFTWARE,
    .timeout_ms = 100,
    .hal        = &hal,
};
spi_init(&bus, &cfg);
```

### 3 — Register a device and transfer

```c
spi_device_t flash;
spi_device_cfg_t dev_cfg = {
    .cs_id    = 0,
    .mode     = SPI_MODE_0,
    .speed_hz = 40000000,
};
spi_register_device(bus, &dev_cfg, &flash);

/* Read 3-byte JEDEC ID from W25Q128 */
uint8_t id[3];
spi_read_cmd(flash, 0x9F, id, 3);
/* id[0]=0xEF (Winbond), id[1]=0x40, id[2]=0x18 */
```

### 4 — Use a device driver

```c
#include "spi_devices.h"

w25q128_t norf;
w25q128_init(&norf, flash_device);

/* Erase sector, write 16 bytes, read back */
w25q128_erase_sector(&norf, 0x000000);
w25q128_write(&norf, 0x000000, data, 16);
w25q128_read(&norf,  0x000000, buf,  16);
```

---

## File Structure

```
spi_lib/
├── include/
│   └── spi.h                    ← Core API header
├── src/
│   └── spi.c                    ← Software bit-bang implementation
├── drivers/
│   ├── spi_devices.h            ← 4 device driver headers
│   └── spi_devices.c            ← 4 device driver implementations
├── examples/
│   └── example_all_devices.c   ← Full demo: all drivers on one bus
├── .github/workflows/build.yml  ← CI: gcc, clang, ARM cross-compile
├── CMakeLists.txt
├── CHANGELOG.md
├── CONTRIBUTING.md
└── LICENSE
```

---

## SPI Clock Modes

| Mode | CPOL | CPHA | Devices |
|------|------|------|---------|
| 0 | 0 | 0 | W25Q128, MCP3204, SD card |
| 1 | 0 | 1 | MAX31855, MAX31865 |
| 2 | 1 | 0 | Rare |
| 3 | 1 | 1 | ST7735, ST7789, ILI9341 |

---

## API Reference

### Bus

| Function | Description |
|----------|-------------|
| `spi_init(bus, cfg)` | Initialize bus |
| `spi_deinit(bus)` | Release resources |
| `spi_register_device(bus, dev_cfg, device)` | Register a device with its own config |

### Transaction

| Function | Description |
|----------|-------------|
| `spi_transaction_begin(device)` | Assert CS, begin transfer |
| `spi_transaction_end(device)` | Release CS, end transfer |

### Core transfers

| Function | Description |
|----------|-------------|
| `spi_write(device, data, len)` | Transmit only |
| `spi_read(device, data, len)` | Receive only |
| `spi_transfer(device, xfer)` | Full-duplex |
| `spi_transfer8/16/32(device, tx, rx)` | Typed word transfers |
| `spi_write_cmd(device, cmd, data, len)` | Command + data |
| `spi_read_cmd(device, cmd, data, len)` | Command + receive |

---

## Build

```bash
# CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make

# Direct gcc
gcc -std=c11 -Wall -I include -I drivers \
    src/spi.c drivers/spi_devices.c examples/example_all_devices.c \
    -o demo -lm
```

---

## Roadmap

- [ ] **v1.1** — FreeRTOS mutex protection (`SPI_ENABLE_RTOS`)
- [ ] **v1.1** — DMA transfer support (STM32 HAL)
- [ ] **v1.2** — Async / callback API
- [ ] **v1.2** — More device drivers: ILI9341, nRF24L01, MCP2515 CAN
- [ ] **v2.0** — Hardware SPI peripheral abstraction

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Bug reports → GitHub Issues. New device driver → PR against `drivers/`.

---

## License

MIT — see [LICENSE](LICENSE).

---

## References

- [SPI specification](https://www.analog.com/en/analog-dialogue/articles/introduction-to-spi-interface.html)
- [W25Q128 datasheet](https://www.winbond.com/resource-files/w25q128jv%20revf%2003272018%20plus.pdf)
- [MAX31855 datasheet](https://datasheets.maximintegrated.com/en/ds/MAX31855.pdf)
- [MCP3204 datasheet](https://ww1.microchip.com/downloads/en/DeviceDoc/21298e.pdf)
- [ST7735 datasheet](https://www.displayfuture.com/Display/datasheet/controller/ST7735.pdf)
