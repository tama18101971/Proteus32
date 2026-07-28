# AGENTS.md

## Project

**Proteus003** — firmware for a CH32V003F4U6 (RISC-V RV32EC, 48 MHz, 16 KB Flash, 2 KB RAM) flashlight.

## Build & flash

```
pio run                          build
pio run -t clean; pio run        clean rebuild
pio run -t upload                flash via WCH-Link
pio device monitor               115200 8N1
```

Only PlatformIO. No IDE, no Makefile, no CMake.

## ⚠️ CH32V003 ≠ STM32

Never apply STM32 knowledge. Registers can have same names but different behavior. CH32V003 specifics: AWU exists, only USART1, I2C CKCFGR formula differs, GPIO CFGLR masks Mode via `& 0x0F` (see [GPIO pitfall](#gpio-cfglr-pitfall) below). Use `GPIO_PinConfig()` helper from `ch32v003_gpio.h` for safe register writes.

## Architecture

### Timers (TIM2) — 1 ms system tick

```
SYSCLK = 48 MHz
PSC = 47  → 1 MHz timer clock
ARR = 999 → 1 ms period
```

Main loop polls tick helpers (not ISR-driven beyond `tim2_ms` increment):

| Rate  | Interval | What runs                             |
|-------|----------|---------------------------------------|
| 160Hz | 6 ms     | `button_tick() + ui_process() + ss_scan()` |
| 30Hz  | 33 ms    | `adc_read() + scan_limiters()`        |
| 2Hz   | 500 ms   | debug UART log                        |
| 5s    | 5 s      | `handle_ee()` then `need_sleep = true`|

`tim2_stop()` resets all tick counters — must re-initialize after sleep.

### Sleep model — two flags

- **`need_sleep`**: request to enter STANDBY next loop iteration. Set by 5s timer, cleared by button ISR.
- **`is_sleep_mode`**: `true` after `AWU_Sleep_Now()`, `false` after button wake restore.

Before STANDBY: `adc_stop() + tim2_stop()`. On button wake: `adc_init() + tim2_start() + dac_pwrpin_init() + i2c_init()`.
`AWU_IRQHandler` calls `SystemInit()` **only** when `is_sleep_mode == true`.

### Button FSM (`button.c/h`)

Debounced state machine at 160 Hz. Emits events (`btn_evt_t`): PRESSED, RELEASED, CLICK_SEQ, HOLD. Knows nothing about driver modes or UI — pure click/hold abstraction. Multi-click sequences up to 7 (or 31 in menus), with per-click long-press bitmask.

### UI dispatch (`ui_table.c/h`)

Table-driven: `dispatch_t` entries map `(driver_mode + btn_evt_t)` to action functions via `main_op[]` function pointer table. One dispatch table per UI type (ramping/UI1, discrete/UI2, engineering menu, user menu).

### Soft start (`soft_start.c`)

Step-based brightness transitions. `ss_init(target, current, speed)` computes direction + step count. `ss_scan()` is called at 160 Hz. Speeds: `SS_NO=0, SS_SLOW=1, SS_FAST=7`. Calls `out_set_stc()` then `OFF_POWER()` when brightness reaches 0.

### DAC (`dac.c/h`)

External DAC7571 (12-bit) via I2C. Pins: PC1=SDA, PC2=SCL, PD0=power control. `dac_pwrpin_on/off()` controls PD0. DAC output drives the LED brightness (mapped from 9-bit `step` value to 12-bit DAC value).

### LED GPIO (`led_gpio.c/h`)

GPIO direct-drive (not PWM): PC4=green, PC7=red (push-pull), PC6=red low-side (pull-up). `on_red_h/l`, `off_red` — no PWM, sets pin mode between output-high, input-pullup, and output-low for brightness stepping.

### Thermal limiter (`thermal_limiter.c/h`)

PI controller with `max(P, I)` arbitration. All integer shift-based (no multiply/divide). 3 zones relative to `t_threshold = T_SET - t_anticipate_offset`. See `TUNING.md` for full tuning guide. **6 constraints listed below — do not violate without approval.**

Config struct (`config` in `limiters.c`) is `const` flash-resident. State (`state`) is RAM.

### Data storage

- **WL_FLASH**: wear-leveling EEPROM over flash. Fields accessed via `EE.fieldname` macro (`app_data.h` overlay on `ee_data`).
- **Option Bytes** (`ob.c/h`): separate from WL_FLASH. Cached in RAM, flushed explicitly via `ob_Flush()`. Contains 10 slots (`ob_GetSlot/SetSlot`) + counters (`ml`, `tc`, `vc`).

### AUX LEDs (`aux_leds.c/h`)

State machine for blinking red/green/backlight LEDs via AWU timer. Commands are 32-bit packed words (`MAKE_CMD` or `MAKE_CMD_MS` macros) encoding device, timing, brightness, repeat count. `MAKE_CMD_MS` takes milliseconds instead of raw psc/win pairs — prefer it. Processed in `handle_aux()` during AWU wake cycles. Strobe patterns live in `aux_leds.c` (`strobe[4]`).

## Critical config

| File | Purpose |
|------|---------|
| `include/proteus_config.h` | `#define WL_FLASH_DEBUG`, `EE_PRINTF`, `USE_*` feature flags |
| `include/app_data.h` | EE overlay + `EE` macro |
| (library) `config.h` | WL_FLASH params (EEPROM_PAGE_COUNT, EE_STRUCT_SIZE) — not shadowed by project |
| `src/limiters.c` | `T_SET`, `config` — thermal limiter tuning |
| `include/thermal_limiter.h` | Compile-time constants (`THERMAL_*`), `ThermalLimiterConfig_t` |

Feature toggles in `proteus_config.h`:
- `USE_BATTIND`, `USE_STROBE`, `USE_BLOCK`, `USE_MENU`, `USE_MAGIC_SLOTS`, `USE_EX_AUX`

## Style

- C99, `stdint.h` fixed-width types — never bare `int`/`short`/`long`
- snake_case for fields/locals, PascalCase for public API (`ThermalLimiter_*`)
- `#ifndef` guards, timeouts on all `while` loops
- Comments explain **why**, not what. Russian for `main.c`, English for thermal limiter
- Register writes: `RCC->APB2PCENR |= RCC_AFIOEN` (direct, not SDK helpers)

## Thermal limiter constraints

1. No float/double
2. No multiply/divide in `ThermalLimiter_Update` — only shifts
3. `ThermalLimiterState_t` fields are int16_t or smaller; int32_t transient only
4. `Limiter = max(P, I)`, never `P + I`
5. Slew-rate filter is final authority on visible brightness change speed
6. Failsafe (`t_raw == 0 || t_raw >= 1023`): immediate bypass, PWM=0, no debounce
7. `max_limiter` hard ceiling ensures minimum brightness floor (PWM >= user_pwm - max_limiter)

## GPIO CFGLR pitfall

`CH32V003` only has one `CFGLR` register per port (8 pins × 4 bits). SDK masks Mode via `& 0x0F`:
```c
#define CFG_IN_PULL   0x08  // input pull-up, Mode=00, CNF=10
#define CFG_OUT_PP_2M 0x02  // output push-pull 2MHz, Mode=10, CNF=00
```
Do NOT write raw AF_OD values like `0x1C` — the `& 0x0F` mask in `GPIO_PinConfig` will truncate them. Use the helpers from `ch32v003_gpio.h`.
