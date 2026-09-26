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
pio run -e Proteus32-RGB         RGB-индикатор (USE_IND_RGB=1: PC7=R, PC4=G, PC6=B;
                                 exaux выключен, раскладка EE.aux своя — старой в этом
                                 варианте не существует, WL_FORMAT_VERSION общий)
```

## ⚠️ Hardware constraints

- **CH32V003 ≠ STM32**: same register names, different behavior (AWU exists, only USART1, I2C CKCFGR differs). GPIO CFGLR masks Mode via `& 0x0F` in SDK helpers — **use `GPIO_PinConfig()` (`mini_gpio.h`)**, never raw AF_OD like `0x1C`.
- **Clock: 24 MHz native HSI, no PLL/APB dividers** (`RCC_CFGR0`: only `HPRE`/`ADCPRE`/`SW`/`PLLSRC`) → HCLK = PCLK1 = PCLK2 = SYSCLK. `SYSCLK_HZ` (`include/proteus_config.h`) is the single source for TIM2 `PSC`/`ARR`, I2C1 `FREQ`/`CCR`, USART1 `BRR`, `SPIN_LOOPS_MS()`; pinned by `_Static_assert` in `timers.c`/`i2c.c`/`mini_printf.c`. **Keep `SYSCLK_HZ` in sync with `board_build.f_cpu` (`platformio.ini`) by hand** — `f_cpu` picks the actual `SetSysClockTo_*()`, mismatch compiles silently.
- **Flash limit strict (16128 B)**: current **16124 B** (`Proteus32`) / **16048 B** (`Proteus32-RGB`), i.e. 4/80 B free; RAM statics 276 B + 512 B stack. `pio run` prints `Size:` + `RAM:` + Top-15 from `tools/fw_size.py` — inspect both every time. On overflow, disable feature flags in `include/proteus_config.h` (`USE_STROBE`/`USE_BLOCK`/`USE_BATTIND`/`USE_MENU`). `-DUART_DEBUG` no longer fits (expected overflow).
- **Size flags: `-Oz` lives in `build_src_flags`, not `build_flags`.** The ch32v platform appends its own `-Os` to `CCFLAGS` (`_bare.py:64`) while executing `$BUILD_SCRIPT` (`platformio/builder/main.py:173`) — i.e. *after* `build_flags`/`build_unflags` are folded in (`:157`). Last `-O*` on the command line wins, so **`build_flags = -Oz` and `build_unflags = -Os` are both silently ineffective**. `build_src_flags` is applied to `src/*.c` when the project-lib env clone is built (`piolib.py:124` → `ProjectAsLibBuilder.process_extra_options` → `ProcessFlags` *appends*), which is **after** the platform's `-Os` — so it wins. Two more traps found while measuring: (a) link-time (`LINKFLAGS`) optimisation flags are **inert under LTO** — the per-TU level decides, proved by a link-only `-falign-functions=16` producing a byte-identical `.bin`; (b) a `post:` extra_script (the only place where final flag lists are visible, `main.py:180`) **cannot** reach `src/*.c` at all — those are compiled by the clone made back at `:173`. Measured and rejected: `-O1/-O2/-O3` (overflow by 1744/6392/21400 B), `-flto-partition=one|balanced`, `--param max-inline-insns-auto/small=0,inline-unit-growth=60` (+20 B), `-fno-inline-functions-called-once` (+28 B here, +4 B RAM), `-fno-toplevel-reorder` (overflow +1056 B), `-falign-*=1`, `-fmerge-all-constants` (RAM unchanged), `-fno-unwind-tables/-fno-ident`, `-fsingle-precision-constant`, `-fipa-icf` (all 0 B). Note `-flto-partition=1` is an invalid value (valid: `1to1|balanced|max|none|one`). `ASPPFLAGS` must stay untouched: it is only used for the startup asm.
- **RV32EC: no HW mul/div** — avoid 32-bit division/multiplication in hot paths (use bit-shifts `<<`/`>>`, fixed-point math).

## Architecture

- Poll (160 Hz / 6 ms): `button_tick() + ui_process() + ss_scan()`
- Poll (30 Hz / 33 ms): `adc_read() + scan_limiters() + scan_voltage()`
- 2 Hz / 500 ms: debug UART log (`[NORMAL] T=... F=... I=... L=... P=... S=... V=...`)
- 5 s: `wl_ob_flush()` + EE-коммит по diff с тенью (записи только при явных изменениях настроек, см. Data storage); then sleep
- Sleep: `need_sleep` = STANDBY req (boot + 5 s tick; cleared by button wake); `is_sleep_mode` true after `AWU_Sleep_Now()` until button wake restore; при активном драйвере ниже `THERMAL_FAST_STEP` термосон идёт в ритме окон AUX (AWU принадлежит AUX FSM, `thermal_sleep_allowed()` допускает активный AUX), замеры АКБ/термо — на каждом aux-wake; паттерн закончился — возврат к AWU_4S; кнопочное пробуждение при активном AUX не обезоруживает AWU — паттерн продолжается в бодрствовании

### Subsystems

- **Button FSM** (`button.c/h`): 160 Hz debounced FSM → `btn_evt_t` {PRESSED, RELEASED, CLICK_SEQ, HOLD}; multi-click ≤7 (31 in menus) with per-click long-press bitmask.
- **UI dispatch** (`ui_table.c/h`): `dispatch_t` maps `(driver_mode + btn_evt_t)` → actions via `main_op[]` (42 entries, `MAIN_OP_LAST` in `ui_table.h`); one table per UI (UI1/UI2/eng/user). `select_slot()`/`slotN_clickX()` shared for all 10 magic slots — index = `driver_mode - MODE_SLOT01`. **`main_op[]` indices 0..12 are frozen** (0..11 = `FUN_Index_t` slot content, persist-ятся в EE нибблами; 12 = `SLOT_UI_SELECT` = `SLOT_OP_LAST`) — only ≥13 may be renumbered. The table must stay exactly `MAIN_OP_LAST` long (`_Static_assert` in `ui_table.c`), and a `MODE_BLOCK_C` slot op must not appear after index 12. Menu click-entry sub-modes (`MODE_MOON_LEVEL`, `MODE_CTK_H`, `MODE_UIS..MODE_LM_TIME`, `MODE_EAUX`) are **not** table rows: `ui_table_lookup()` matches that range itself and calls `menu_clickX()` directly.
- **⚠️ Lockout modes are NOT a contiguous range.** `DRIVER_Mode_t` interleaves them with normal modes: `MODE_BLOCK=0, MODE_BLOCK0=1, MODE_BLOCK_C=2, MODE_OFF=3, MODE_ON=4, MODE_STROB=5, MODE_BLOCK_ON=6, MODE_BLOCK_LIGHT=7`. The lockout set is `{0,1,6,7}` and is tested by the `BLOCK_MODE_MASK` bitmask in `ui_table.c` — never by `mode <= MODE_BLOCK_LIGHT`, which also swallows the three normal driver modes and deadens the whole UI (the branch ends in an unconditional `return 0`).
- **Save-as-default ack** (`save_def_blink()`, `ui.c`): 5 clicks + hold (UI1/UI2 → `save_def_ui1/2`) — 30 ms light gap; with exaux on (`UIF_EAUX_ONOFF`, user menu item 9) no gap: 2 green flashes (`aux_on()`/`INFO_GRN`, same as unlock).
- **Block/unblock ack** (`slot_block()`/`unblock_drv()`, `ui.c`; slot 5 = 4 quick clicks): with the light on and exaux off — light gap (`blink_light()`, 30 ms, shared with the save ack); with exaux on — unchanged `aux_off()`/`aux_on()` (red/green); light off — red/green in both cases; MODE_BLOCK_LIGHT keeps the old indication.
- **Soft start** (`soft_start.c`): `ss_init(target, current, speed)`, `ss_scan()` @160 Hz; speeds `SS_NO=0, SS_SLOW=1, SS_FAST=7`; `out_set_stc()` → `OFF_POWER()` at 0.
- **DAC** (`dac.c/h`): DAC7571 (12-bit) via I2C (PC1=SDA, PC2=SCL, PD0=pwr ctrl `dac_pwrpin_on/off`); 9-bit `step` → 12-bit via cubic gamma `(step^3) >> 15`. On failure `dac7571_set_voltage()` reinitialises I2C once (`i2c_init()`) — bus timeout cannot wedge writes (audit B1; timeout paths emit STOP).
- **LED GPIO** (`led_gpio.c/h`): direct-drive (no PWM). PC4=green, PC7=red push-pull, PC6=red low-side (pull-up); `on_red_h/l`, `off_red` switch pin mode (output-high/input-pullup/output-low) for brightness.

### Thermal limiter (`thermal_limiter.c/h`, config in `src/limiters.c`, tuning guide in `TUNING.md`)

- Continuous PI controller, linear $P + I$ summing, with **Equilibrium Hold Band** ($T_{\text{set}} - t_{\text{anticipate\_offset}} \le T < T_{\text{set}}$) holding $\Delta I = 0$ → no hunting.
- Fixed-point EMA filter (scale $\times 32$, 5 bits): symmetric round-half-up — `avg += (diff + 2^(shift−1)) >> shift` → settled residual ±(2^(shift−1) − 1) fp-LSB (±7/32 ADC code at shift 4), no one-sided bias; the earlier truncation-toward-−∞ variant was replaced (audit C5 superseded); hunting still excluded (residual ≪ 8-code hold band).
- Integral accumulator (scale $\times 64$, 6 bits) with dynamic anti-windup clamping.
- Slew filter (`max_drop_rate`, `max_recovery_rate`) → flicker-free 30 Hz transitions.

### Temperature calibration

Two layers: **`tc`** (Option Bytes, `OB_CFG->tc` overlay, int8_t HW ADC correction, `get_exact_temp_adc = raw - tc`) and **`t_calibr`** (`EE.usr.t_calibr`, 1–15 clicks = 46–60 °C, default 5 = 50 °C, `get_thermal_setpoint()`). `calibration()` (`ui.c`, eng menu) → Option Bytes.

### Data storage

WL_FLASH v2.4.0 (не вендорится — тянется через `lib_deps` в `platformio.ini`, пин на тег `#v2.4.0`; локальной копии в `libs/` нет, опция `lib_extra_dirs` тоже удалена). С v2.4.0 и текущим образом (7 строб-профилей, без 30/30): **16124 B** (`Proteus32`) / **16048 B** (`Proteus32-RGB`); 2.2.0 занимала 15840/15856 B, то есть апгрейд версии стоил сотни байт: сообщение линковщика вида «overflowed by N bytes» нельзя читать как дельту к текущему `Size:` — это остаток от предела 16128 B, а не разница версий. RGB-вариант влезает благодаря тому, что в нём таблица строб-профилей — массив указателей (нет cycle-бита → все профили массивы, дискриминатор не нужен, `aux_leds.c`). При обновлении сначала проверять `pio run` (гейт `fw_size.py`); 2.3.0 требует `wl_ob_init()` до `wl_ob_flush()` (уже есть в `ob_Init()`), 2.4.0 убирает `wl_scan_t.found` (в прошивке не используется). Записей на флэш: wear-leveling record ring in `0x08003F00..0x08004000` (record 16 B, CRC-16, 4 pages × 4 slots = 16). Payload = `app_cfg_t` (9 B: `ui1` хранит UI1 step>>1, `uif` биты 0..4 флаги + биты 5..7 `default_ui2`, `aux`, `usr`, 10 nibble-packed magic slots) in `app_data.h`; `EE.fieldname` = `(*WL_DATA_AS(app_cfg_t))`. Boot: `wl_load()` → on fail `wl_init() + ee_defaults()`. **Сессия в RAM, конфиг во Flash:** сессионная яркость/режим (`step_default`/`mode_default`) живёт в SRAM (сохраняется Standby при выключенном фонаре); `off_driver()` EE не трогает — при повседневных вкл/выкл записей во Flash **0**. Коммит EE — только на 5-с тике по diff `EE` vs `cfg_shadow` (memcmp): пишут лишь явные действия (5 кликов `save_def_ui1/2`, меню, слоты, флаги, `ee_defaults`). Сбой записи: dirty-флаг остаётся → ретрай на следующем 5-с тике, ≤`WL_MAX_COMMIT_RETRY=3`, latches `WL_RETRY_EXHAUSTED` until reload; OB flush остаётся на 5-с тике. Формат pinned в `platformio.ini` (`-DWL_FORMAT_VERSION=4 -DWL_RECORD_SIZE=16 -DWL_CRC_BITS=16`; bump on payload change); v3→v4 сбрасывает настройки в дефолт (одноразово); `maximum_size = 16128` + `-Wl,--defsym=FLASH_SIZE=16128` + гейт `fw_size.py` ERROR_AT=16128 enforce limits. `WL_FLASH_LIGHT` removed upstream.
- **Option Bytes** (`ob.h` overlay over the `wl_ob_*` module of WL_FLASH v2.4.0): RAM-cached, flushed via `wl_ob_flush()` in the persistence tick; calibration counters only (ml, tc, vc); change idiom `OB_CFG->field = x; wl_ob_mark_dirty();`.

### AUX LEDs / strobes (`aux_leds.c/h`)

AWU-sleep FSM: `handle_aux()` runs **one** transition per AWU wake, then sleeps `psc*win/128` ms. Use `MAKE_CMD`/`MAKE_CMD_MS` (preferred over raw psc/win). Brightness indexes `modes[]`; HBL uses `get_stepi()` — **never** `set_current()` (changes `step_user`, powers down DAC).

`awu_tick_t` (`awu.h`) lists **only** the presets the firmware arms (15 ms, 60 ms, 120 ms, 1 s, 4 s) and indexes `awu_lut[]` in `awu.c` directly; `AWU_set_delay()` is `void` with no range check. A new preset must be added to the enum **and** the table together — the `AWU_TICKS_LAST` sentinel is pinned against `AWU_TICKS_COUNT` by a `_Static_assert` in `awu.c` (plus one for `AWU_IMMEDIATELY` being outside the table), so a preset without a table row fails the build.

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
