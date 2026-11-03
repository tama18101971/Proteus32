# Definition of done
1. `pio run` succeeds AND firmware size < 16128 bytes across all build environments (Proteus32, Proteus32-RGB, Proteus32-MCP4725, Proteus32-RGB-MCP4725) — inspect size every build (flash ~full is normal; on overflow temporarily disable USE_* flags in include/proteus_config.h and report to user).
2. No new compiler warnings.
3. Symbol edits via Serena refactoring tools (rename_symbol / safe_delete_symbol / replace_symbol_body) — on success, no re-read of files to verify; trust the result.
4. UI/dispatch changes: never assign ui_table_idx 5/6 outside boot-menu; dispatch table sizes must stay consistent with main_op[] (MAIN_OP_LAST, 42/43 entries); lockout modes tested via BLOCK_MODE_MASK.
5. Voltage regulation changes: cascade step-down must only decrement mode_current/volt_stop_mode and clamp brightness down (never unconditional set_current); ADC error resets streaks only, keeping ceiling intact; reset_voltage_latches() resets all on driver off.
6. Thermal changes: follow TUNING.md procedure; tuning struct lives in src/limiters.c.
7. Bootloader or flasher protocol changes: verify against protocol rev 2 pytest vectors (test/test_protocol_v2.py).
8. Flash `pio run -t upload`; verify on device via `pio device monitor` (460800 8N1) using the 2 Hz [NORMAL] log line (T/F/I/L/P/S/V).
9. If memories were edited, verify references are intact across the memory graph (`mem:core`).