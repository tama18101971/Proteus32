# Proteus3D — AVR-порт Proteus32 (синхронизация)

C:\Sonet5\Proteus3D — порт Proteus32 (CH32V003) на ATtiny1634 (AVR, 1 МГц, 16 КБ Flash / 1 КБ RAM, PlatformIO env attiny1634, bare-metal).

## Точка синхронизации
- Порт изначально снят с рабочего дерева, включавшего изменения коммита Proteus32 `2037f59` (батарейка-защита с храповиком).
- 2026-08-30: перенесена ВСЯ логика Proteus32 после `2037f59` (вплоть до `c26b30d`): единый потолок UI1/UI2 (ratchet = get_stepi(volt_max_mode)), ERRO_RED при клампах напряжения, турбо-память (ps.use_prev && ps.start), down_forever = menu_entry_flash(1,2000,RED), ex_lvls 3.4/3.2/3.0 В, API aux без параметра enable, немедленный старт стробов/массивов (WDT_IMMEDIATELY), ON_POWER() в start_aux_strobe, guard !EnableArrayProcessing у OFF_POWER, тайминги индикации счёта (152/448 мс), exaux ON 100 мс + старт WDTO_1S, опрос 30 Гц при любом активном драйвере.
- Намеренно НЕ переносится: `ae4c224` (сон без WDT уже нативен на AVR), AWU/LSI-специфика CH32, `d07848d` (ui_table дескрипторы — размерная оптимизация CH32).
- Отличия доменов: напряжение в bandgap-кодах (числитель 1126400 vs 1228800), AUX-тайминги кодами WDTO (0..15) вместо psc/win, поле count 5 бит (0..31), EEPROM своя (myeeprom.c, struct eeblock) вместо WL_FLASH/Option Bytes.
- UI-действия живут в src/button.c (не ui.c), main loop — src/proteus.c.
- Сборка: `pio run` (C:\Sonet5\Proteus3D); после порта 2026-08-30: Flash 13896/16384 (84.8%), RAM 216/1024. Фичи-флаги в include/macros.h (USE_STROBE/MENU/BATTIND/EX_AUX/BLOCK/MAGIC_SLOTS, REAL_OFFPOWER — все включены).
- При следующих изменениях логики Proteus32 — проверять дельту после `c26b30d` (`git log 2037f59..HEAD` в Proteus32) и портировать по той же схеме: логика — да, платформенно-специфичное (AWU/LSI/Option Bytes/I2C-драйвер) — адаптировать.
