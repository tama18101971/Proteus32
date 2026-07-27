# AGENTS.md

## Project

**Proteus003** — firmware for a CH32V003F4U6 (RISC-V RV32EC, 48 MHz, 16 KB Flash, 2 KB RAM) flashlight.

- Build only via PlatformIO: `pio run`, `pio run -t clean; pio run`, `pio run -t upload`
- monitor: `pio device monitor` (115200 8N1)
- LTO enabled (`board_build.use_lto = yes`)
- Two external libs via `lib_deps`: I2C-CH32V003, WL_FLASH

## ⚠️ CH32V003 ≠ STM32

Never use STM32 knowledge. Registers can have the same names but different behavior. Check CH32V003 datasheet, not STM32. Specific differences well-known: AWU exists, only USART1, I2C CKCFGR formula is different, GPIO CFGLR masks Mode via `& 0x0F`, etc.

## Key architecture

- **Data storage**: WL_FLASH library (wear-leveling EEPROM over flash). Application data defined as `app_data.h` — `union ee_view` overlays the library's raw `ee_data`. Access app fields via `EE.fieldname` (macro-based).
- **Option Bytes** (`ob.c`/`ob.h`): separately handled via OB cache (`ob_GetMl`/`ob_SetMl`, `ob_GetSlot`/`ob_SetSlot`). NOT in the WL_FLASH struct.
- **Thermal limiter** (`thermal_limiter.c`): PI controller with `max(P, I)` arbitration, integer-only, zero-multiplication (shift-based only). Constraints documented in `thermal_limiter.h` — honor them.
- **Sleep**: STANDBY mode with AWU. Deinit ADC, TIM2, I2C before sleep. AWU_IRQHandler calls `SystemInit()` only when `is_sleep_mode == true`.
- **UART debug**: `EE_PRINTF()` via `mini_printf`, enabled by `#define WL_FLASH_DEBUG` in `proteus_config.h`

## Critical config

- `include/proteus_config.h` — project-specific defines (WL_FLASH_DEBUG, EE_PRINTF, USE_* flags)
- `include/app_data.h` — EE overlay + EE macro
- The library has its own `config.h` (for EEPROM_PAGE_COUNT, EE_STRUCT_SIZE etc.), no longer shadowed after renaming project's from config.h → proteus_config.h
- `build_flags` in `platformio.ini` are minimal (`-Iinclude`, linker defs) — WL_FLASH params come from its own config.h defaults

## Commands

```
pio run                          build
pio run -t clean; pio run        clean rebuild
pio run -t upload                flash via WCH-Link
pio device monitor               115200 8N1
```

## Style

- C99, `stdint.h` fixed-width types (`uint8_t`, `int16_t`, `uint32_t`) — never bare `int`/`short`/`long` for ADC/PWM/state
- snake_case for fields/locals, PascalCase for public API (`ThermalLimiter_*`), `THERMAL_*` for macros
- Register writes: `RCC->APB2PCENR |= RCC_AFIOEN` (direct, no SDK helpers except in `main.c`)
- `#ifndef` guards on headers, timeouts on all `while` loops
- Comments explain **why**, not what. Russian for `main.c`, English for `thermal_limiter.*`
- When changing configurable behavior (ThermalLimiterConfig_t, zone boundaries, debug log format) — update `TUNING.md`

## Thermal limiter constraints (do not violate without user approval)

1. No float/double — integer fixed-point only
2. No multiply/divide in hot path (`ThermalLimiter_Update`) — only shifts (`<<`, `>>`)
3. `ThermalLimiterState_t` fields are int16_t or smaller. int32_t only as temporary in expressions, never stored.
4. `Limiter = max(P, I)`, never `P + I`
5. Slew-rate filter (`max_drop_rate`/`max_recovery_rate`) is the final authority on visible brightness change speed
6. Failsafe (`t_raw == 0 || t_raw >= 1023`): immediate bypass, PWM=0, print `E_THERMAL_RUNAWAY`. No debounce.

## GPIO CFGLR pitfall

SDK masks Mode via `& 0x0F`:
```c
// AF_OD (0x1C) → 0x0C, then |= Speed (0x03) → 0x0F
#define GPIO_CFG_AF_OD_30M  0x0F  // do NOT write 0x1C directly
```
