# AGENTS.md

## Project

**Proteus003** — firmware for a CH32V003F4U6 (RISC-V RV32EC, 48 MHz, 16 KB Flash, 2 KB RAM) flashlight.

## Build & flash

```
pio run                          build
pio run -t clean; pio run        clean rebuild
pio run -t upload                flash via WCH-Link
pio device monitor               460800 8N1
```

Only PlatformIO. No IDE, no Makefile, no CMake.
UART init: our own `uart_init()` in `mini_printf.c` (bare-metal register writes), **not** SDK's `USART_Printf_Init`.

## ⚠️ CH32V003 ≠ STM32

Never apply STM32 knowledge. Registers can have same names but different behavior. CH32V003 specifics: AWU exists, only USART1, I2C CKCFGR formula differs, GPIO CFGLR masks Mode via `& 0x0F` (see [GPIO pitfall](#gpio-cfglr-pitfall) below). Use `GPIO_PinConfig()` helper from `mini_gpio.h` for safe register writes.

## Architecture

### Timers (TIM2) — 1 ms system tick

```
SYSCLK = 48 MHz
PSC = 47  → 1 MHz timer clock
ARR = 999 → 1 ms period
```

Main loop polls tick helpers (not ISR-driven beyond `tim2_ms` increment):

| Rate  | Interval | What runs                                        |
|-------|----------|--------------------------------------------------|
| 160Hz | 6 ms     | `button_tick() + ui_process() + ss_scan()`       |
| 30Hz  | 33 ms    | `adc_read() + scan_limiters() + scan_voltage()`  |
| 2Hz   | 500 ms   | debug UART log                                   |
| 5s    | 5 s      | `ob_Flush()` + EE shadow-diff → `handle_ee()` if changed; then sleep decision |

`tim2_stop()` resets all tick counters — must re-initialize after sleep.

### Sleep model — two flags

- **`need_sleep`**: request to enter STANDBY next loop iteration. Set by 5s timer (and at boot, `main.c`), cleared by button ISR/wake.
- **`is_sleep_mode`**: `true` after `AWU_Sleep_Now()`, `false` after button wake restore.
- **TIM2 is NOT started at boot**: `main()` never calls `tim2_start()`. Boot → `need_sleep=true` → immediate STANDBY; the first button wake restores peripherals and starts the 1 ms tick, which drives the whole 160/30/2 Hz polled loop. Blocking helpers (`wait_ms`, 3 s boot delay, boot-menu flashes) run before STANDBY and don't need timers.

Before STANDBY: `adc_stop() + tim2_stop()`. On button wake: `dac_pwrpin_restore() + i2c_init(400000) + adc_init() + tim2_start()`. On AWU wake: restore DAC/I2C **only if** `aux_needs_dac()` (strobe/HBL active), then `handle_aux()`.
`AWU_IRQHandler` calls `SystemInit()` **only** when `is_sleep_mode == true`.

### Button FSM (`button.c/h`)

Debounced state machine at 160 Hz. Emits events (`btn_evt_t`): PRESSED, RELEASED, CLICK_SEQ, HOLD. Knows nothing about driver modes or UI — pure click/hold abstraction. Multi-click sequences up to 7 (or 31 in menus), with per-click long-press bitmask.

### UI dispatch (`ui_table.c/h`)

Table-driven: `dispatch_t` entries map `(driver_mode + btn_evt_t)` to action functions via `main_op[]` function pointer table (`MAIN_OP_COUNT = RESET2DEF + 1` = 49 entries). One dispatch table per UI type (ramping/UI1, discrete/UI2, engineering menu, user menu).
`select_slot()`/`slotN_clickX()` are shared for all 10 magic slots — slot index is derived from `driver_mode - MODE_SLOT01` (or `clicks_count - 1`).

### Soft start (`soft_start.c`)

Step-based brightness transitions. `ss_init(target, current, speed)` computes direction + step count. `ss_scan()` is called at 160 Hz. Speeds: `SS_NO=0, SS_SLOW=1, SS_FAST=7`. Calls `out_set_stc()` then `OFF_POWER()` when brightness reaches 0.

### DAC (`dac.c/h`)

External DAC7571 (12-bit) via I2C. Pins: PC1=SDA, PC2=SCL, PD0=power control. `dac_pwrpin_on/off()` controls PD0. DAC output drives the LED brightness (mapped from 9-bit `step` value to 12-bit DAC value).

### LED GPIO (`led_gpio.c/h`)

GPIO direct-drive (not PWM): PC4=green, PC7=red (push-pull), PC6=red low-side (pull-up). `on_red_h/l`, `off_red` — no PWM, sets pin mode between output-high, input-pullup, and output-low for brightness stepping.

### Thermal limiter (`thermal_limiter.c/h`)

PI controller with `max(P, I)` arbitration. All integer shift-based (no multiply/divide). 3 zones relative to `t_threshold = BASE_TEMPERATURE + t_calibr - t_anticipate_offset`. See `TUNING.md` for full tuning guide. **6 constraints listed below — do not violate without approval.**

Config struct (`config` in `limiters.c`) is `const` flash-resident. State (`state`) is RAM.

### Battery protection (`scan_voltage()` in `adc.c`)

Protects against deep discharge of 1S Li-ion. Uses `mode_swt[6]` thresholds `{2700, 2750, 2800, 2900, 3000, 3100}` mV.
- **UI1 (ramping)**: hard cutoff at 2700 mV, step-down at each threshold with 25% brightness reduction per step.
- **UI2 (discrete)**: automatic mode reduction, hard cutoff if already at mode 0.
- `volt_max_mode`/`volt_stop_mode` globals in `adc.c` track current/historical voltage ceiling for discrete modes.

### Temperature calibration

Two-layer calibration:
- **`tc`** (`ob_GetTc()`/`ob_SetTc()`): hardware ADC correction (int8_t) stored in Option Bytes. Applied in `get_exact_temp_adc()` = `raw_temperature_adc - tc`, consumed by `calc_temperature()` and `ThermalLimiter_Update()`.
- **`t_calibr`** (`EE.usr.t_calibr`): user-adjustable setpoint offset (4-bit, 0–15) from engineering menu (`chtk`). Dynamically computed: `t_set = BASE_TEMPERATURE + t_calibr`.
- `BASE_TEMPERATURE` = termo_table[70] (50°C, ADC=722) minus default t_calibr (6) = 716.
- `calibration()` in `ui.c` (engineering menu) measures current Vbat/temperature, computes vc/tc offsets, saves to Option Bytes.

### Data storage

- **WL_FLASH**: wear-leveling EEPROM over flash. Fields accessed via `EE.fieldname` macro (`app_data.h` overlay on `ee_data`). Uses `WL_FLASH_LIGHT` variant (see platformio.ini).
- **Deferred EE writes — shadow gate in main.c**: `ee` is the RAM working copy; `ee_shadow` (main.c:46) is the last-persisted snapshot. The 5s tick runs `ob_Flush()` (no-op if OB cache clean) then `memcmp(&ee_shadow, &ee, EE_STRUCT_SIZE)`: only on difference does it set `need_save=1`, call `handle_ee()` (whole 16-byte struct), and refresh `ee_shadow` on success; a failure keeps `need_save=1` for retry. **Do NOT set `need_save = 1` from UI code** — the tick's memcmp is the single authority. An even number of toggles inside a 5s window restores the old value and writes nothing.
- **Session vs persisted state**: `step_default`/`mode_default` are RAM-only session values, mutated constantly (thermal dim in `set_current_ll`, turbo preview, `off_driver_false`, strobe exit). Never write transient values into `EE.*` — the 5s tick will persist them to flash; the memcmp gate filters "no change", not "don't persist". Sync points: `ui_init()` loads `EE.default_ui1/2` → `step_default`/`mode_default`; `off_driver()` copies RAM → EE **only when** `EE.uif.use_memory`.
- **Option Bytes** (`ob.c/h`): separate from WL_FLASH. Cached in RAM, flushed explicitly via `ob_Flush()`. Contains 10 slots (`ob_GetSlot/SetSlot`) + counters (`ml`, `tc`, `vc`).

### AUX LEDs / strobes (`aux_leds.c/h`)

AWU-sleep-driven FSM: `handle_aux()` executes **one** transition per AWU wake (main loop `WS_AWU` branch), then MCU sleeps `psc*win/128` ms. Commands are 32-bit words (`MAKE_CMD`/`MAKE_CMD_MS`): `device[2:0]`, `cycle[3]`, `count[7:4]`, `psc0[10:8]`, `win0[16:11]`, `psc1[19:17]`, `win1[25:20]`, `brightness0[28:26]`, `brightness1[31:29]`. Brightness indexes `modes[]` levels; HBL uses `get_stepi()` — never `set_current()` (it changes `step_user` and powers down the DAC).

- **`MAKE_CMD_MS(device, cycle, count, ms0, ms1, b0, b1)`** preferred over raw psc/win. `AWU_MS_PSC` boundaries: 63/127/254/508/1016/2048/5160 ms, saturates at 30240 ms. `ms=0` → `win=0` = "must not wait" (avoids the AWU delay). Multi-phase arrays pair phases like SOS does — avoid bare `ms1=0` off-phases.
- **Arrays**: elements are LED commands or system commands `AC_STOP` (end), `AC_REP_ATSTART` (loop from index 0), `AC_REP_ATSTEPS` (loop from `steps` field). A finite (`cycle=0`, count→0) HBL command ends dark — next array element loads in the same wake (no visible gap).
- **Strobe profiles**: `strobe_commands[STROBE_COUNT]` in aux_leds.c — 4 single-command profiles + arrays `strobe_sos[]`, `strobe_bike2[]`. Adding a profile = new array (or packed command) + descriptor + bump `STROBE_COUNT` in aux_leds.h; `ui.c` strobe cycling wraps automatically.
- **Strobe owns the DAC**: while `is_aux_mode(IDM_STROBE)`, main loop skips `adc_read()/scan_limiters()/scan_voltage()`. Strobe normally sleeps between AWU phases; a button wake keeps the FSM running through the click timeout so the first press isn't lost (gate at main.c:144).

### Engineering / user menus

Actions gated by `#ifdef USE_MENU` (bodies; the dispatch tables are always compiled). **Only entry: power-up with the button held** — `enter_menu_at_boot()` (`ui.c`) is called from `main()` when `Pressed` at startup. Menu is chosen by supply voltage (like original AVR Proteus IID):
- V > `ENG_VOLTAGE_MV` (4.5 V, bench/USB): `vrefint_adc < MV_GE(ENG_VOLTAGE_MV)` → engineering menu (`ENG_MENU_IND`=5), indicator flashes **6× red**;
- else (battery) → user menu (`USR_MENU_IND`=6), **3× green**.
Entry holds the button ~1 s (`MENU_ENTRY_DEBOUNCE_MS`), then flashes, then waits for release so the button FSM doesn't fire HOLD→`MENU_EXIT` on the first tick.

Menus are `ui_table_idx` **5/6** (see `ui_table_lookup`) — **session-only, never persisted**: reboot returns to the normal UI; `menu_exit()` restores `ui_table_idx = ui_save`. Menu actions fire only on `CLICK_SEQ` (inter-click pause ~250 ms / `BTN_MAX_CLICKS_MENU`=31); **hold** in MODE_OFF or any sub-mode → exit (`MENU_EXIT` / `EXIT_MAGIC_MODE` / slot-hold special case in `ui_process`).
- `eng_table`: 1=enter ML, 2/3=`save_def_ui1/2` (always compiled, not USE_MENU), 10=`calibration()` (tc/vc → Option Bytes), 20=`RESET2DEF`, hold=exit.
- `usr_table`: 1=BM, 2=BT, 3=BL, 4=UIS (clamped to valid 1..3 in `uis_clickX`), 5=MAGIC slots, 6=CHTK, hold=exit.
- `menu_beacon_mute` (RAM flag): suppresses the AUX beacon for the whole menu session (checked in `start_aux_beacon()`); set by `enter_menu_at_boot()`, cleared by `menu_exit()`. Never touches `EE.aux.beacon_on` — a power-cycle inside the menu can't disable the beacon permanently.

## Critical config

| File | Purpose |
|------|---------|
| `include/proteus_config.h` | Feature flags (`USE_*`), `EE_PRINTF`, UART debug toggles |
| `include/app_data.h` | EE overlay struct + `EE` macro (used everywhere) |
| `include/adc.h` | `REF_TEMP_ADC`, `REF_VOLTAGE_MV`, `ADC_MIN/MAX_VAL`, `VC/TC_CLIP_*` |
| `include/limiters.h` | `BASE_TEMPERATURE`, `TERMO_50C_ADC`, `TCALIBR_DEFAULT` |
| (library) `config.h` | WL_FLASH params — not shadowed by project |
| `src/limiters.c` | `config` struct — thermal limiter PID tuning |
| `include/thermal_limiter.h` | Compile-time constants (`THERMAL_*`), `ThermalLimiterConfig_t` |

Feature toggles in `proteus_config.h`:
- `USE_BATTIND`, `USE_STROBE`, `USE_BLOCK`, `USE_MENU`, `USE_MAGIC_SLOTS` — enabled
- `USE_EX_AUX` — present but commented out (off by default)

## Style

- C99, `stdint.h` fixed-width types — never bare `int`/`short`/`long`
- snake_case for fields/locals, PascalCase for public API (`ThermalLimiter_*`)
- `#ifndef` guards, timeouts on all `while` loops
- Comments explain **why**, not what. Russian for `main.c`, English for thermal limiter
- Register writes: `RCC->APB2PCENR |= RCC_AFIOEN` (direct, not SDK helpers)
- **BARE METAL > SDK**: All GPIO, UART, timer and peripheral code must use direct register writes (`port->CFGLR`, `USART1->DATAR`, etc.). Never replace bare-metal code with SDK helper calls. The `mini_gpio.h` helpers are acceptable for new GPIO code, but existing bare-metal inline functions must not be rewritten to use them.
- **Function headers**: Every function (public and static) must have a Doxygen-style header (`/** @brief ... */`) immediately above its definition. When modifying a function's signature, parameters, or behavior, update its header to match — never leave stale or missing documentation.

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
Do NOT write raw AF_OD values like `0x1C` — the `& 0x0F` mask in `GPIO_PinConfig` will truncate them. Use the helpers from `mini_gpio.h`.

## Gotchas

- **`ui_table_idx` valid values are only 1 (UI1), 2/3 (UI2), 5/6 (menus)**: `ui_table_lookup` (ui_table.c) returns 0 for anything else → dead UI (no button action, unrecoverable without reboot). When touching UI/menu selection (e.g. `uis_clickX`), clamp to 1..3; never assign 5/6 outside the boot-menu flow.
- **`EE_PRINTF` is a no-op when `WL_FLASH_DEBUG` is off**: arguments are never evaluated (macro `#define EE_PRINTF(...) ((void)0)`). Code inside its argument list (e.g. `T_SET`, `EE.usr.*`) won't cause compile errors when debug is off, but will break if debug is re-enabled.
- **`adc.c` must include `ui.h`**: needed for `CMODE_*`, `driver_mode`, `mode_current`, `step_user`, `off_driver()`, `set_current()`, `get_stepi()` (used in `scan_voltage`).
- **`calibration()` is under `#ifdef USE_MENU`**: only accessible via engineering menu. Writes to Option Bytes (not WL_FLASH).
- **`mini_printf`** is ~1.5 KB when linked — disable `WL_FLASH_DEBUG` to reclaim it.
- **`select_slot()`/`slotN_clickX()` in `ui.c`** are shared for all 10 slots; slot index is computed at runtime from `driver_mode - MODE_SLOT01` (or `clicks_count - 1`). Do NOT create per-slot functions.
- **`uart_init()`** in `mini_printf.c` uses direct register writes (not SDK helpers). `out_char()` uses `USART1->DATAR`/`USART1->STATR` directly — keep SDK-free if modifying UART code.
