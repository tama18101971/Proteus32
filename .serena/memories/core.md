# Proteus32 — CH32V003F4U6 flashlight firmware (RISC-V RV32EC, 48 MHz, 16 KB Flash, 2 KB RAM)

## Hard invariants
- Flash cap 16128 bytes; builds run near 98–100% with LTO. Always check size after `pio run`. On overflow temporarily disable flags in include/proteus_config.h: USE_STROBE, USE_BLOCK, USE_BATTIND, USE_MENU.
- RV32EC has no HW mul/div: no 32-bit division/multiplication in hot paths — shifts + fixed point only (scales in use: temp EMA ×32, PI integral ×64, DAC gamma (step^3)>>15).
- CH32V003 ≠ STM32: same register names, different behavior (AWU quirks, only USART1, I2C CKCFGR differs). Configure GPIO via GPIO_PinConfig() from mini_gpio.h (masks Mode with &0x0F); never raw AF_OD like 0x1C.
- Bare metal > SDK: direct register writes (port->CFGLR, USART1->DATAR); do not rewrite existing bare-metal inline funcs.
- Timeouts on all while loops; #ifndef header guards; C99 stdint fixed-width only (never bare int/short/long).

## Source map (src/ + include/, .c/.h pairs 1:1)
- main.c — main loop, poll scheduler on TIM2 1 ms tick, sleep orchestration (need_sleep / is_sleep_mode, button wake)
- button.c + button_gpio.c — 160 Hz debounced FSM → btn_evt_t {PRESSED, RELEASED, CLICK_SEQ, HOLD}; multi-click ≤7 (31 in menus) + per-click long-press bitmask
- ui.c + ui_table.c — dispatch_t maps (driver_mode + btn_evt_t) → actions via main_op[] (49 entries); one table per UI (UI1/UI2/eng-menu/user-menu); select_slot()/slotN_clickX() shared for 10 magic slots, index = driver_mode - MODE_SLOT01
- soft_start.c — step-based: ss_init/ss_scan @160 Hz; SS_NO=0, SS_SLOW=1, SS_FAST=7; out_set_stc() → OFF_POWER() at 0
- dac.c — external DAC7571 via I2C (PC1=SDA, PC2=SCL, PD0=pwr ctrl); 9-bit step → 12-bit via cubic gamma (step^3)>>15
- led_gpio.c — direct GPIO drive: PC4=green, PC7=red push-pull, PC6=red low-side pull-up; on_red_h/l, off_red (pin mode switching for brightness)
- thermal_limiter.c + limiters.c — continuous PI + Equilibrium Hold Band; EMA temp ×32 with symmetric rounding, integral ×64 with anti-windup, slew-rate filters (max_drop_rate/max_recovery_rate); tuning values in src/limiters.c `config` struct; procedure in TUNING.md
- adc.h — REF_TEMP_ADC, REF_VOLTAGE_MV, ADC_MIN/MAX_VAL
- aux_leds.c — FSM driven by AWU wakes, ONE transition per wake then sleep; commands via MAKE_CMD/MAKE_CMD_MS (never raw psc/win); HBL brightness via get_stepi(), NEVER set_current()
- ob.c — Option Bytes RAM cache (10 slots + counters ml/tc/vc), flushed via ob_Flush(); calibration() writes Option Bytes, NOT WL_FLASH
- app_data.h — WL_FLASH EEPROM overlay: EE.fieldname over ee_data; WL_FLASH_LIGHT/WL_FLASH_DEBUG defines live in platformio.ini (commented out by default)
- mini_printf.c — compact printf for UART debug 460800 8N1

## Poll schedule (TIM2 1 ms tick)
- 160 Hz (6 ms): button_tick + ui_process + ss_scan
- 30 Hz (33 ms): adc_read + scan_limiters + scan_voltage
- 2 Hz: debug log `[NORMAL] T=.. F=.. I=.. L=.. P=.. S=.. V=..`
- 5 s: ob_Flush + EE shadow-diff → handle_ee, then sleep

## In-repo docs
AGENTS.md (architecture + gotchas), TUNING.md (thermal limiter tuning), USER_MANUAL.md (UI gestures/slots)

## Deeper topics
- Build/flash commands incl. Windows quirks and stale paths: `mem:suggested_commands`
- Toolchain, PlatformIO env, libs: `mem:tech_stack`
- Code style, naming, doc rules: `mem:conventions`
- Definition of done incl. flash-size gate: `mem:task_completion`