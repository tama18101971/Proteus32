# Definition of done
1. `pio run` succeeds AND firmware size < 16128 bytes — inspect size every build (flash ~full is normal; if overflow → temporarily disable USE_* flags in include/proteus_config.h and report to user)
2. No new compiler warnings
3. Symbol edits via Serena refactoring tools (rename_symbol / safe_delete_symbol / replace_symbol_body) — on success, no re-read of files to verify; trust the result
4. UI/dispatch changes: never assign ui_table_idx 5/6 outside boot-menu; dispatch table sizes must stay consistent with main_op[] (49 entries)
5. Thermal changes: follow TUNING.md procedure; tuning struct lives in src/limiters.c
6. Flash `pio run -t upload`; verify on device via `pio device monitor` (460800) using the 2 Hz [NORMAL] log line (T/F/I/L/P/S/V)
7. If memories were edited, mention that `serena memories check` can validate references