# AGENTS.md

## Project

Proteus32 firmware for CH32V003F4U6 (RISC-V RV32EC, 24 MHz, 16 KB Flash, 2 KB RAM) flashlight.

## UART bootloader (protocol rev 2)

- Bootloader repo `Proteus32-BootLoader`, spec `BOOTLOADER_TZ.md` (protocol v2, WL_FLASH-safe). Boot area: **separate 1920-B flash @ 0x00000000** (mapped only in BOOT start); user code @ 0x08000000 untouched.
- **`proteus32_3.py` = protocol rev 2 (v2.1), pairs ONLY with bootloader rev 2** (old pairs incompatible: VERIFY-with-data, READ 0x84, statuses 0x02/0x03). `PROTOCOL_REV = 2`; pytest vectors: `test/test_protocol_v2.py` (byte-exact).
- Write window **only** `[0x08000000, 0x08003F00)`; WL_FLASH region `0x08003F00..0x08004000` **never** written/erased; no `--force`; image ≤ 16128 B (script + bootloader guarded). Entry: power-on with button held (PC3→GND). Script timeouts: ERASE 10 s, else 3 s, 3 retries/frame.

## Build & flash

```
pio run                          build
pio run -t clean; pio run        clean rebuild
pio run -t upload                flash via WCH-Link
pio device monitor               460800 8N1
```

## ⚠️ Hardware constraints

- **CH32V003 ≠ STM32**: same register names, different behavior (AWU exists, only USART1, I2C CKCFGR differs). GPIO CFGLR masks Mode via `& 0x0F` in SDK helpers — **use `GPIO_PinConfig()` (`mini_gpio.h`)**, never raw AF_OD like `0x1C`.
- **Clock: 24 MHz native HSI, no PLL/APB dividers** (`RCC_CFGR0`: only `HPRE`/`ADCPRE`/`SW`/`PLLSRC`) → HCLK = PCLK1 = PCLK2 = SYSCLK. `SYSCLK_HZ` (`include/proteus_config.h`) is the single source for TIM2 `PSC`/`ARR`, I2C1 `FREQ`/`CCR`, USART1 `BRR`, `SPIN_LOOPS_MS()`; pinned by `_Static_assert` in `timers.c`/`i2c.c`/`mini_printf.c`. **Keep `SYSCLK_HZ` in sync with `board_build.f_cpu` (`platformio.ini`) by hand** — `f_cpu` picks the actual `SetSysClockTo_*()`, mismatch compiles silently.
- **Flash limit strict (16128 B)**: code ~98-100% with LTO; inspect size on `pio run`. On overflow, disable feature flags in `include/proteus_config.h` (`USE_STROBE`/`USE_BLOCK`/`USE_BATTIND`/`USE_MENU`). `-DUART_DEBUG` no longer fits (expected overflow).
- **RV32EC: no HW mul/div** — avoid 32-bit division/multiplication in hot paths (use bit-shifts `<<`/`>>`, fixed-point math).

## Architecture

- Poll (160 Hz / 6 ms): `button_tick() + ui_process() + ss_scan()`
- Poll (30 Hz / 33 ms): `adc_read() + scan_limiters() + scan_voltage()`
- 2 Hz / 500 ms: debug UART log (`[NORMAL] T=... F=... I=... L=... P=... S=... V=...`)
- 5 s: `ob_Flush()` + EE-коммит по diff с тенью (записи только при явных изменениях настроек, см. Data storage); then sleep
- Sleep: `need_sleep` = STANDBY req (boot + 5 s tick; cleared by button wake); `is_sleep_mode` true after `AWU_Sleep_Now()` until button wake restore; при активном драйвере ниже `THERMAL_FAST_STEP` термосон идёт в ритме окон AUX (AWU принадлежит AUX FSM, `thermal_sleep_allowed()` допускает активный AUX), замеры АКБ/термо — на каждом aux-wake; паттерн закончился — возврат к AWU_4S; кнопочное пробуждение при активном AUX не обезоруживает AWU — паттерн продолжается в бодрствовании

### Subsystems

- **Button FSM** (`button.c/h`): 160 Hz debounced FSM → `btn_evt_t` {PRESSED, RELEASED, CLICK_SEQ, HOLD}; multi-click ≤7 (31 in menus) with per-click long-press bitmask.
- **UI dispatch** (`ui_table.c/h`): `dispatch_t` maps `(driver_mode + btn_evt_t)` → actions via `main_op[]` (49 entries); one table per UI (UI1/UI2/eng/user). `select_slot()`/`slotN_clickX()` shared for all 10 magic slots — index = `driver_mode - MODE_SLOT01`.
- **Soft start** (`soft_start.c`): `ss_init(target, current, speed)`, `ss_scan()` @160 Hz; speeds `SS_NO=0, SS_SLOW=1, SS_FAST=7`; `out_set_stc()` → `OFF_POWER()` at 0.
- **DAC** (`dac.c/h`): DAC7571 (12-bit) via I2C (PC1=SDA, PC2=SCL, PD0=pwr ctrl `dac_pwrpin_on/off`); 9-bit `step` → 12-bit via cubic gamma `(step^3) >> 15`. On failure `dac7571_set_voltage()` reinitialises I2C once (`i2c_init()`) — bus timeout cannot wedge writes (audit B1; timeout paths emit STOP).
- **LED GPIO** (`led_gpio.c/h`): direct-drive (no PWM). PC4=green, PC7=red push-pull, PC6=red low-side (pull-up); `on_red_h/l`, `off_red` switch pin mode (output-high/input-pullup/output-low) for brightness.

### Thermal limiter (`thermal_limiter.c/h`, config in `src/limiters.c`, tuning guide in `TUNING.md`)

- Continuous PI controller, linear $P + I$ summing, with **Equilibrium Hold Band** ($T_{\text{set}} - t_{\text{anticipate\_offset}} \le T < T_{\text{set}}$) holding $\Delta I = 0$ → no hunting.
- Fixed-point EMA filter (scale $\times 32$, 5 bits): `diff >> shift` truncates toward $-\infty$ → settles up to $2^{\text{filter\_shift}} - 1$ fp-LSB (≤15/32 ADC code) below true value (one-sided cold bias, anti-hunting; integer stays unbiased); kept over symmetric rounding to save Flash (audit C5).
- Integral accumulator (scale $\times 64$, 6 bits) with dynamic anti-windup clamping.
- Slew filter (`max_drop_rate`, `max_recovery_rate`) → flicker-free 30 Hz transitions.

### Temperature calibration

Two layers: **`tc`** (Option Bytes, `ob_GetTc/SetTc`, int8_t HW ADC correction, `get_exact_temp_adc = raw - tc`) and **`t_calibr`** (`EE.usr.t_calibr`, 1–15 clicks = 46–60 °C, default 5 = 50 °C, `get_thermal_setpoint()`). `calibration()` (`ui.c`, eng menu) → Option Bytes.

### Data storage

WL_FLASH v2.0.1 (vendored `libs/WL_FLASH`): wear-leveling record ring in `0x08003F00..0x08004000` (record 16 B, CRC-16, 4 pages × 4 slots = 16). Payload = `app_cfg_t` (9 B: `ui1` хранит UI1 step>>1, `uif` биты 0..4 флаги + биты 5..7 `default_ui2`, `aux`, `usr`, 10 nibble-packed magic slots) in `app_data.h`; `EE.fieldname` = `(*WL_DATA_AS(app_cfg_t))`. Boot: `wl_load()` → on fail `wl_init() + ee_defaults()`. **Сессия в RAM, конфиг во Flash:** сессионная яркость/режим (`step_default`/`mode_default`) живёт в SRAM (сохраняется Standby при выключенном фонаре); `off_driver()` EE не трогает — при повседневных вкл/выкл записей во Flash **0**. Коммит EE — только на 5-с тике по diff `EE` vs `cfg_shadow` (memcmp): пишут лишь явные действия (5 кликов `save_def_ui1/2`, меню, слоты, флаги, `ee_defaults`). Сбой записи: dirty-флаг остаётся → ретрай на следующем 5-с тике, ≤`WL_MAX_COMMIT_RETRY=3`, latches `WL_RETRY_EXHAUSTED` until reload; OB flush остаётся на 5-с тике. Формат pinned в `platformio.ini` (`-DWL_FORMAT_VERSION=4 -DWL_RECORD_SIZE=16 -DWL_CRC_BITS=16`; bump on payload change); v3→v4 сбрасывает настройки в дефолт (одноразово); `maximum_size = 16128` + `-Wl,--defsym=FLASH_SIZE=16128` + гейт `fw_size.py` ERROR_AT=16128 enforce limits. `WL_FLASH_LIGHT` removed upstream.
- **Option Bytes** (`ob.c/h`): RAM-cached, flushed via `ob_Flush()`; calibration counters only (ml, tc, vc); magic slots moved to WL_FLASH v3.

### AUX LEDs / strobes (`aux_leds.c/h`)

AWU-sleep FSM: `handle_aux()` runs **one** transition per AWU wake, then sleeps `psc*win/128` ms. Use `MAKE_CMD`/`MAKE_CMD_MS` (preferred over raw psc/win). Brightness indexes `modes[]`; HBL uses `get_stepi()` — **never** `set_current()` (changes `step_user`, powers down DAC).

### Engineering / user menus

`#ifdef USE_MENU` gates bodies; tables always compiled. **Entry:** power-up with button held → `enter_menu_at_boot()` (`ui.c`) when `Pressed`. Voltage-gated: bench (>4.5 V) → eng menu (idx 5); batt → user (idx 6).

## Critical config & docs

- `include/proteus_config.h`: `SYSCLK_HZ`, `SPIN_LOOPS_MS()`, feature flags (`USE_*`), debug toggles `UART_DEBUG`/`PRINT_ADC`
- `include/app_data.h`: EE overlay + `EE` macro · `include/adc.h`: `REF_TEMP_ADC`, `REF_VOLTAGE_MV`, `ADC_MIN/MAX_VAL`
- `src/limiters.c`: thermal `config` · `TUNING.md`: tuning guide · `USER_MANUAL.md`: UI gestures & slots
- `proteus32_3.py`: PC flasher v2.1 (rev 2): write / `--erase-only` / `--dump` / `--go`
- `test/test_protocol_v2.py`: pytest vectors (TZ 3.5.4) — `python -m pytest test/test_protocol_v2.py -v`

## Style

- C99, `stdint.h` fixed-width — never bare int/short/long. snake_case locals/fields, PascalCase API (`ThermalLimiter_*`).
- **Serialized 32-bit command words** (e.g. AUX commands in `aux_leds.h`) must NEVER be decoded via C bitfields (ABI layout). Use explicit `AUX_*_MASK`/`AUX_*_SHIFT` macros + `AUX_xxx()` accessors, pinned with `_Static_assert` (no overlap/gaps, round-trip, golden values). Bitfields OK only for internal, never-serialised structs.
- `#ifndef` guards; timeouts on all `while` loops.
- **Bare metal > SDK**: direct register writes (`port->CFGLR`, `USART1->DATAR`). `mini_gpio.h` OK for new GPIO — don't rewrite existing bare-metal inline funcs.
- **Doxygen headers** (`/** @brief ... */`) above every function — update with signature/behavior.

## Gotchas

- **`ui_table_idx`** valid only 1 (UI1), 2/3 (UI2), 5/6 (menus); else `ui_table_lookup` returns 0 → dead UI (reboot only). Clamp to 1..3 outside boot-menu; never assign 5/6 except at boot.
- **`calibration()`** writes Option Bytes, not WL_FLASH.
- **`select_slot()`/`slotN_clickX()`** shared for all 10 slots; index = `driver_mode - MODE_SLOT01` (or `clicks_count - 1`). Don't create per-slot functions.
