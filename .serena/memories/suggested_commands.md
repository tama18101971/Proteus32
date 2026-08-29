# Commands (Windows / PowerShell environment)
- `pio run` — build; ALWAYS inspect flash size in output (cap 16128 B, near 100% is normal here)
- `pio run -t clean; pio run` — clean rebuild (run sequentially)
- `pio run -t upload` — flash via WCH-Link
- `pio device monitor` — serial 460800 8N1 (2 Hz debug line is the health check)
- `pio run -t compiledb` — regenerate compile_commands.json (project root) after build-flag changes; clangd consumes it via `.clangd` (silences IncludeCleaner stderr spam, improves symbol resolution)
- `build_exe.cmd` — rebuild PC flasher (PyInstaller) → dist/Proteus32Flasher.exe
- `flash_tool.cmd` — flash firmware.bin via Proteus32Flasher.exe; WARNING: hardcoded paths point to E:\GitProjects\Proteus32 (stale — project actually lives at C:\Sonet5\Proteus32); fix paths before using
- Agent note: shell is PowerShell (run_commands tool) — use Get-ChildItem/Select-String instead of ls/grep; avoid `>` redirection for file writes, prefer Set-Content or [System.IO.File]::WriteAllText; batch stateful read-modify-write into a SINGLE command (parallel command batches have raced before)
- When editing files outside the workspace (e.g. Cline config in %USERPROFILE%\.cline), filesystem MCP is restricted to C:\Sonet5 — use run_commands instead