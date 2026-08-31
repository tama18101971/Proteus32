# AGENTS.md

## Project

Proteus32 firmware for CH32V003F4U6 (RISC-V RV32EC, 48 MHz, 16 KB Flash, 2 KB RAM) flashlight.

## Build & flash

```
pio run                          build
pio run -t clean; pio run        clean rebuild
pio run -t upload                flash via WCH-Link
pio device monitor               460800 8N1
```

## ⚠️ Hardware & Resource Constraints

- **CH32V003 ≠ STM32**: Same register names, different behavior. AWU exists, only USART1, I2C CKCFGR differs. GPIO CFGLR masks Mode via `& 0x0F` in SDK helpers — **use `GPIO_PinConfig()` from `mini_gpio.h`**, never raw AF_OD like `0x1C`.
- **Flash limit is strict (16128 bytes)**: Codebase runs near ~98-100% Flash capacity with LTO. Always inspect binary size on `pio run`. If flash overflows during feature development, feature flags in `include/proteus_config.h` (`USE_STROBE`, `USE_BLOCK`, `USE_BATTIND`, `USE_MENU`) can be temporarily disabled.
- **RV32EC Core**: No hardware multiplier or divider. Avoid software 32-bit division/multiplication in hot paths (use bit-shifts `<<`, `>>` and fixed-point math).

## Architecture

### Poll rates (TIM2 1 ms tick)

| Rate | Interval | Runs |
|------|----------|------|
| 160 Hz | 6 ms | `button_tick() + ui_process() + ss_scan()` |
| 30 Hz | 33 ms | `adc_read() + scan_limiters() + scan_voltage()` |
| 2 Hz | 500 ms | debug UART log (`[NORMAL] T=... F=... I=... L=... P=... S=... V=...`) |
| 5 s | 5 s | `ob_Flush()` + EE shadow-diff → `handle_ee()` if changed; then sleep |

### Sleep model

- `need_sleep`: STANDBY request; set at boot and by 5 s tick; cleared by button wake.
- `is_sleep_mode`: true after `AWU_Sleep_Now()`, false after button wake restore.

### Subsystems

- **Button FSM** (`button.c/h`): 160 Hz debounced FSM emitting `btn_evt_t` {PRESSED, RELEASED, CLICK_SEQ, HOLD}. Multi-click up to 7 (31 in menus) with per-click long-press bitmask.
- **UI dispatch** (`ui_table.c/h`): `dispatch_t` maps `(driver_mode + btn_evt_t)` → actions via `main_op[]` (49 entries). One table per UI (UI1/UI2/eng-menu/user-menu). `select_slot()`/`slotN_clickX()` shared for all 10 magic slots — index from `driver_mode - MODE_SLOT01`.
- **Soft start** (`soft_start.c`): step-based. `ss_init(target, current, speed)`, `ss_scan()` @160 Hz. Speeds `SS_NO=0, SS_SLOW=1, SS_FAST=7`. `out_set_stc()` → `OFF_POWER()` at 0.
- **DAC** (`dac.c/h`): external DAC7571 (12-bit) via I2C. PC1=SDA, PC2=SCL, PD0=pwr ctrl (`dac_pwrpin_on/off`). Maps 9-bit `step` → 12-bit DAC via cubic gamma curve `(step^3) >> 15`.
- **LED GPIO** (`led_gpio.c/h`): direct-drive (no PWM). PC4=green, PC7=red (push-pull), PC6=red low-side (pull-up). `on_red_h/l`, `off_red` — pin mode between output-high/input-pullup/output-low for brightness.

### Thermal limiter (`thermal_limiter.c/h`, config in `src/limiters.c`, tuning guide in `TUNING.md`)

- Continuous PI controller with linear $P + I$ summing and **Equilibrium Hold Band** ($T_{\text{set}} - t_{\text{anticipate\_offset}} \le T < T_{\text{set}}$) holding $\Delta I = 0$ to eliminate hunting/oscillations.
- Fixed-point EMA temperature filter (scale $\times 32$, 5 fractional bits) with symmetric rounding to eliminate integer truncation bias ($F == T$ at steady state).
- Fixed-point integral accumulator (scale $\times 64$, 6 fractional bits) with dynamic anti-windup clamping.
- Slew rate filter (`max_drop_rate`, `max_recovery_rate`) ensuring flicker-free transitions at 30 Hz.

### Temperature calibration

Two layers: **`tc`** (Option Bytes, `ob_GetTc/SetTc`, int8_t HW ADC correction, `get_exact_temp_adc = raw - tc`) and **`t_calibr`** (`EE.usr.t_calibr`, 1–15 clicks = 46–60 °C, default 5 = 50 °C, `t_set = get_thermal_setpoint()`). `calibration()` in `ui.c` (eng menu) → Option Bytes.

### Data storage

WL_FLASH: wear-leveling EEPROM over flash, `EE.fieldname` via `app_data.h` overlay on `ee_data` (`WL_FLASH_LIGHT` variant).
- **Option Bytes** (`ob.c/h`): cached in RAM, flushed via `ob_Flush()`. 10 slots + counters (ml, tc, vc).

### AUX LEDs / strobes (`aux_leds.c/h`)

AWU-sleep-driven FSM: `handle_aux()` runs **one** transition per AWU wake, then MCU sleeps `psc*win/128` ms. Commands via `MAKE_CMD`/`MAKE_CMD_MS` (preferred over raw psc/win). Brightness indexes `modes[]`; HBL uses `get_stepi()` — **never** `set_current()` (changes `step_user`, powers down DAC).

### Engineering / user menus

`#ifdef USE_MENU` gates bodies; dispatch tables always compiled. **Entry: power-up with button held** → `enter_menu_at_boot()` (`ui.c`) from `main()` when `Pressed`. Voltage-gated: bench (V>4.5 V) → eng menu (idx 5); batt → user menu (idx 6).

## Critical config & docs

| File | Purpose |
|------|---------|
| `include/proteus_config.h` | Feature flags (`USE_*`), `UART_DEBUG`/`PRINT_ADC` debug toggles |
| `include/app_data.h` | EE overlay + `EE` macro |
| `include/adc.h` | `REF_TEMP_ADC`, `REF_VOLTAGE_MV`, `ADC_MIN/MAX_VAL` |
| `src/limiters.c` | `config` struct (thermal tuning) |
| `TUNING.md` | Practical step-by-step thermal limiter tuning guide |
| `USER_MANUAL.md` | UI gestures, click sequences, and slot configuration |

## Style

- C99, `stdint.h` fixed-width — never bare int/short/long. snake_case locals/fields, PascalCase API (`ThermalLimiter_*`).
- **Serialized 32-bit command words** (stored in Flash, e.g. AUX commands in `aux_leds.h`) must NEVER be decoded via C bitfields (ABI-dependent layout). Use explicit `AUX_*_MASK`/`AUX_*_SHIFT` macros and `AUX_xxx()` accessors, pinned by `_Static_assert` (no overlap, no gaps, encode/decode round-trip, golden values). C bitfields are fine only for internal, never-serialised state structs.
- `#ifndef` guards, timeouts on all `while` loops.
- **Bare metal > SDK**: direct register writes (`port->CFGLR`, `USART1->DATAR`). `mini_gpio.h` OK for new GPIO; don't rewrite existing bare-metal inline funcs.
- **Doxygen headers** (`/** @brief ... */`) above every function — update when signature/behavior changes.

## Gotchas

- **`ui_table_idx`** valid only: 1 (UI1), 2/3 (UI2), 5/6 (menus). `ui_table_lookup` returns 0 for anything else → dead UI (reboot only). Clamp to 1..3 outside boot-menu; never assign 5/6 except at boot.
- **`calibration()`** Writes Option Bytes, not WL_FLASH.
- **`select_slot()`/`slotN_clickX()`** shared for all 10 slots; index = `driver_mode - MODE_SLOT01` (or `clicks_count - 1`). Don't create per-slot functions.
