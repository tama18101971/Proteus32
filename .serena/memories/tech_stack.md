# Tech stack
- C99 bare-metal firmware, WCH CH32V003F4U6 (RV32EC, 48 MHz)
- PlatformIO: platform=ch32v, board=genericCH32V003F4U6, framework=noneos-sdk (WCH SDK, not ch32v003fun)
- board_build.use_lto=yes; custom linker script Link.ld; -Wl,--defsym FLASH_SIZE=16128, RAM_SIZE=2048; board_upload.maximum_size=16128
- build_flags: -Iinclude; -U__PIO_CPP_SUPPORT__; WL_FLASH_DEBUG / WL_FLASH_LIGHT defines present but commented out in platformio.ini
- External lib (lib_deps): WL_FLASH wear-leveling EEPROM, github.com/tama18101971/WL_FLASH.git — cloned via sysenv.GITHUB_TOKEN (private repo; token must be set in env)
- PC flasher tool: proteus32_3.py → PyInstaller onefile → dist/Proteus32Flasher.exe (built by build_exe.cmd; Python 3.12 + PyInstaller on system)
- Serena language server: cpp (clangd); works without compile_commands.json on this project
- git 2.54 on PATH; no package manager for firmware deps