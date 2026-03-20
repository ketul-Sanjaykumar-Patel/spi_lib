# Changelog

All notable changes to this project will be documented in this file.
Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html)

---

## [Unreleased]

### Planned
- FreeRTOS mutex protection for thread-safe bus access
- DMA transfer mode for STM32 hardware SPI
- Async/callback API for non-blocking transfers
- ILI9341 display driver (320×240 color TFT)
- nRF24L01 wireless transceiver driver
- MCP2515 CAN bus controller driver
- Hardware SPI peripheral abstraction layer

---

## [1.0.0] - 2024-01-01

### Added
- Software (bit-bang) SPI implementation supporting all 4 clock modes (CPOL/CPHA)
- Per-device configuration: each device stores its own CS pin, mode, speed, bit order
- Device registration system — `spi_register_device()` with automatic bus reconfiguration
- Explicit transaction API: `spi_transaction_begin()` / `spi_transaction_end()`
- Typed transfer functions: `spi_transfer8()`, `spi_transfer16()`, `spi_transfer32()`
- Transfer descriptor (`spi_xfer_t`) for DMA-ready design
- Command helpers: `spi_write_cmd()`, `spi_read_cmd()`
- Bus statistics: tx/rx bytes, transfers, errors, CS toggles
- `spi_err_to_string()` for human-readable error messages
- Static instance pool (no heap allocation)
- W25Q128 NOR Flash driver: read, write (page splits), erase sector/chip, JEDEC ID, sleep/wake
- MAX31855 thermocouple driver: 32-bit frame parse, fault detection (OC/SCG/SCV)
- MCP3204 12-bit ADC driver: single-ended and differential, all 4 channels, millivolt conversion
- ST7735 TFT display driver: init sequence, fill, pixel, rect, blit, rotation, RGB565
- CMakeLists.txt for CMake builds
- GitHub Actions CI: gcc/clang (c11/c99), ARM Cortex-M4 cross-compile, cppcheck
