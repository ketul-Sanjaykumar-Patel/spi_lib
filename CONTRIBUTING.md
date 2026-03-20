# Contributing to spi_lib

## Ways to contribute

- **Bug report** — open a GitHub Issue with the bug report template
- **New device driver** — add under `drivers/`
- **Platform HAL example** — add under `examples/hal/`
- **Documentation** — fix typos, improve comments

## Code style

- C11 standard, no compiler extensions
- Snake_case for all identifiers
- `spi_` prefix for all public symbols
- All public functions documented with `@brief`, `@param`, `@return`
- No dynamic memory allocation in library code
- Return `spi_err_t` on every function — never assert/abort

## Adding a device driver

1. Add header (typedefs, constants, function prototypes) to `drivers/spi_devices.h`
2. Add implementation to `drivers/spi_devices.c`
3. Add usage example to `examples/example_all_devices.c`
4. Update device table in `README.md`
5. Add entry under `[Unreleased]` in `CHANGELOG.md`

Each driver must:
- Verify chip identity on `_init()` (JEDEC ID or WHO_AM_I equivalent)
- Return `spi_err_t` on every function
- Document the SPI mode and max speed required

## Pull request checklist

- [ ] Compiles with `gcc -Wall -Wextra -std=c11` without warnings
- [ ] All public API additions documented in header
- [ ] `CHANGELOG.md` updated under `[Unreleased]`
- [ ] Example updated if new driver added

## Commit messages

```
feat(drivers): add ILI9341 TFT display driver
fix(spi): correct mode 2 clock idle state
docs(readme): add RP2040 HAL example
```
