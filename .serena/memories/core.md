# Proteus32 — CH32V003F4U6 flashlight firmware (RISC-V RV32EC, 24 MHz native HSI, 16 KB Flash, 2 KB RAM)

## Hard invariants
- Flash cap 16128 bytes; builds run near 98–100% with LTO. Current sizes: Proteus32 16000 B (128 B free), Proteus32-RGB 15848 B (280 B free), Proteus32-MCP4725 16000 B, Proteus32-RGB-MCP4725 15848 B. Always check size after `pio run`. On overflow temporarily disable flags in include/proteus_config.h: USE_STROBE, USE_BLOCK, USE_BATTIND, USE_MENU.
- RV32EC has no HW mul/div: no 32-bit division/multiplication in hot paths — shifts + fixed point only (scales in use: temp EMA ×32, PI integral ×64, DAC gamma (step^3)>>15).
- CH32V003 ≠ STM32: same register names, different behavior (AWU quirks, only USART1, I2C CKCFGR differs). Configure GPIO via GPIO_PinConfig() from mini_gpio.h (masks Mode with &0x0F); never raw AF_OD like 0x1C.
- Clock: 24 MHz native HSI, no PLL/APB dividers (HCLK = PCLK1 = PCLK2 = SYSCLK). SYSCLK_HZ in include/proteus_config.h must match board_build.f_cpu in platformio.ini.
- Bare metal > SDK: direct register writes (port->CFGLR, USART1->DATAR); do not rewrite existing bare-metal inline funcs.
- Timeouts on all while loops; #ifndef header guards; C99 stdint fixed-width only (never bare int/short/long).

## Source map (src/ + include/, .c/.h pairs 1:1)
- main.c — main loop, poll scheduler on TIM2 1 ms tick, sleep orchestration (need_sleep / is_sleep_mode, button wake)
- button.c + button_gpio.c — 160 Hz debounced FSM → btn_evt_t {PRESSED, RELEASED, CLICK_SEQ, HOLD}; multi-click ≤7 (31 in menus) + per-click long-press bitmask
- ui.c + ui_table.c — dispatch_t maps (driver_mode + btn_evt_t) → actions via main_op[] (42 entries, 43 with USE_EX_AUX=1, MAIN_OP_LAST); one table per UI (UI1/UI2/eng-menu/user-menu); select_slot()/slotN_clickX() shared for 10 magic slots, index = driver_mode - MODE_SLOT01. Indices 0..12 are frozen (0..11 FUN_Index_t, 12 SLOT_UI_SELECT). Menu click-entry sub-modes are not table rows (called directly in ui_table_lookup). Lockout modes {0, 1, 6, 7} are non-contiguous, tested by BLOCK_MODE_MASK.
- soft_start.c — step-based: ss_init/ss_scan @160 Hz; SS_NO=0, SS_SLOW=1, SS_FAST=7; out_set_stc() → OFF_POWER() at 0
- dac.c — external DAC7571 (0x4C) or MCP4725 (0x60) via I2C (PC1=SDA, PC2=SCL, PD0=pwr ctrl, USE_DAC_MCP4725); 9-bit step → 12-bit via cubic gamma (step^3)>>15. Service utility tools/mcp4725_init flushes EEPROM 0 V to prevent 50% power-on preflash.
- led_gpio.c — direct GPIO drive: PC4=green, PC7=red push-pull, PC6=red low-side pull-up; on_red_h/l, off_red (pin mode switching for brightness). In RGB build (USE_IND_RGB=1): PC7=red, PC4=green, PC6=blue.
- thermal_limiter.c + limiters.c — continuous PI + Equilibrium Hold Band; EMA temp ×32 with symmetric rounding, integral ×64 with anti-windup, slew-rate filters (max_drop_rate/max_recovery_rate); tuning values in src/limiters.c `config` struct; procedure in TUNING.md
- adc.c + adc.h — REF_TEMP_ADC, REF_VOLTAGE_MV, ADC_MIN/MAX_VAL. Undervoltage cascade (swt_cut[1..5] 3.01/2.81/2.74/2.725/2.71 V on EMA), 2.70 V cutoff (swt_cut[0] on raw), 2.50 V deep-discharge backstop (UV_DEEP_MV on raw). Mode cascade decrements mode_current/volt_stop_mode and clamps down (never unconditional set_current). ADC error resets confirmation streaks only (reset_voltage_streaks), keeping volt_stop_mode intact. Latches reset via reset_voltage_latches() on off_driver(). down_forever() turns off LED/DAC, flashes red for 2 s, turns off red, and enters permanent STANDBY.
- aux_leds.c — FSM driven by AWU wakes, ONE transition per wake then sleep; commands via MAKE_CMD/MAKE_CMD_MS (never raw psc/win); HBL brightness via get_stepi(), NEVER set_current().
- ob.h — Option Bytes RAM cache overlay over wl_ob_* of WL_FLASH v2.4.0 (no ob.c); flushed via wl_ob_flush() in 5 s tick; calibration counters only (ml, tc, vc); calibration() writes OB, never WL_FLASH.
- app_data.h — WL_FLASH v2.4.0 payload: app_cfg_t (9 B, packed: ui1, uif, aux, usr, slots_packed[5]); record 16 B, CRC-16, -DWL_FORMAT_VERSION=4. EE.fieldname = (*WL_DATA_AS(app_cfg_t)). Session brightness/mode in SRAM; Flash commit on 5 s tick by diff with cfg_shadow (0 writes on normal on/off).
- mini_printf.c — compact printf for UART debug 460800 8N1 (dead-stripped when UART_DEBUG undefined).

## Poll schedule (TIM2 1 ms tick)
- 160 Hz (6 ms): button_tick + ui_process + ss_scan
- 30 Hz (33 ms): adc_read + scan_limiters + scan_voltage
- 2 Hz: debug log `[NORMAL] T=.. F=.. I=.. L=.. P=.. S=.. V=..`
- 5 s: wl_ob_flush + payload shadow-diff → commit to flash if dirty; then sleep

## In-repo docs
AGENTS.md (architecture + gotchas), TUNING.md (limiter tuning + voltage cascade), USER_MANUAL.md (UI gestures/slots), BOOTLOADER_TZ.md (protocol v2 spec)

## Deeper topics
- Build/flash commands, pio path, flasher v2: `mem:suggested_commands`
- Boot menu selection by supply voltage + ADC plausibility window (5 V bench): `mem:menu_entry_voltage`
- Toolchain, PlatformIO envs (Proteus32, RGB, MCP4725), libs: `mem:tech_stack`
- Code style, naming, doc rules, mode_current gotchas: `mem:conventions`
- Flash size optimization history and techniques: `mem:flash_size_optimization`
- Definition of done incl. flash-size gate and multi-env build: `mem:task_completion`
- AUX command 32-bit layout: `mem:aux_cmd_bit_layout`
- Porting to AVR (Proteus3D): `mem:proteus3d_port`