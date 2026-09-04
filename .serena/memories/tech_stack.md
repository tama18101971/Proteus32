# Tech stack
- C99 bare-metal firmware, WCH CH32V003F4U6 (RV32EC, 24 MHz native HSI — see mem:core for the 48→24 switch)
- PlatformIO: platform=ch32v, board=genericCH32V003F4U6, framework=noneos-sdk (WCH SDK, not ch32v003fun)
- board_build.use_lto=yes; custom linker script Link.ld; -Wl,--defsym FLASH_SIZE=16128, RAM_SIZE=2048; board_upload.maximum_size=16128
- build_flags: -Iinclude; -U__PIO_CPP_SUPPORT__; -DWL_FORMAT_VERSION=2 (WL_FLASH_DEBUG not used — ~5.3 KB of printf does not fit)
- External lib (lib_deps): WL_FLASH v2.0.1 wear-leveling EEPROM, github.com/tama18101971/WL_FLASH.git#v2.0.1 — private repo, cloned via sysenv.GITHUB_TOKEN (token must be set in env)
- PC flasher tool: proteus32_3.py → PyInstaller onefile → dist/Proteus32Flasher.exe (built by build_exe.cmd; Python 3.12 + PyInstaller on system)
- Serena language server: cpp (clangd). Works with compile_commands.json + `.clangd` at project root (DB includes all src/*.c; .clangd strips gcc-only flags and adds fallback include paths/defines for SDK TUs whose entry is rejected by clangd).
- git 2.54 on PATH; no package manager for firmware deps