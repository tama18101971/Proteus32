# Анализ уменьшения Flash: inline/noinline и флаги оптимизации

Дата: 2026-08-30. База: Flash 15936/16128 (98.8%), RAM 536/2048, GCC 12.2.0 (riscv-wch-elf), `-Os` + LTO + `-msave-restore` + `--gc-sections`. Контекст: после удаления мёртвого параметра `enable` из `start_aux_cmd()` (src/aux_leds.c) прошивка выросла +36 Б — это и стало толчком к анализу.

## Главный вывод
**Снижать Flash через `__attribute__((noinline))`/`inline` бесполезно — подтверждено 7 контрольными сборками.** Инлайн-решения GCC (-Os+LTO) в этом проекте оптимальны в обе стороны: каждая попытка вмешаться дала рост или ноль.

## Эксперименты (все с чистой пересборкой, замер Flash)
| Эксперимент | Flash | Δ |
|---|---|---|
| База -Os+LTO | 15936 | — |
| noinline `out_char` (9 вызовов) | 15936 | 0 (функция в мёртвом коде, см. ниже) |
| noinline ×5: GPIO_PinConfig(×21), enter_submode(×10), stop_aux_if(×9), aux_refresh_backlight(×3), ss_cancel(×3) | OVERFLOW | > +192 ❌ |
| noinline только GPIO_PinConfig | OVERFLOW | > +192 ❌ |
| -fno-inline (весь инлайн off) | OVERFLOW | > +192 ❌ |
| -Oz вместо -Os | 15936 | 0 (код байт-в-байт тот же) |
| -flto-partition=none | 16072 | +136 ❌ |

## Механизм (почему noinline вредит)
- Инлайн-клоны здесь **не дублируют, а сжимают** код: вызовы с константными аргументами
  (`enter_submode(MODE_BT, …)`, `GPIO_PinConfig(GPIOA, GPIO_Pin_1, CFG_IN_ANALOG)`) при инлайне
  сворачиваются в ~10 Б, что **меньше, чем подготовка 3 регистров + call** для noinline-вызова (~14 Б).
  Универсальная noinline-функция обрабатывает все случаи + 21×~14 Б на вызовы → рост.
- Обратное направление тоже закрыто: GCC уже инлайнит все single-caller функции
  (`ss_scan` — 1 вызов в main.c:368; `ss_init` — 1 вызов ui.c:288; `start_aux_array` — 2 вызова,
  констант-пропагированные клоны) и правильно держит отдельными большие multi-caller:
  `ui_table_lookup.isra.0` 1104 Б (3 вызова), `handle_aux_cmd.constprop.0` 368 Б (3),
  `handle_aux.isra.0` 116 Б (3), `main` 4028 Б.
- Констант-пропагация: GCC сам создаёт `.constprop.N`-клоны, когда все вызовы передают константу
  (пример: `start_aux_cmd` с enable=true — параметр исчезал ещё на этапе компиляции).

## Топ функций по размеру (для будущих работ по экономии)
main 4028, ui_table_lookup.isra.0 1104, scan_voltage 690, sample_and_scan 528,
handle_aux_cmd.constprop.0 368, AWU_Sleep_Now 262, SystemInit 230, handle_aux_array.constprop.0 228,
main_op 220, adc_init 220, adc_read 214, start_aux_beacon 208, slot_batt_ind 194, dac7571_set_voltage.isra.0 190, set_current_ll 184.

## Важные находки по сборке (PlatformIO ch32v)
- **`build_flags` НЕ может перекрыть уровень оптимизации**: платформа добавляет `-Os` в
  CCFLAGS и LINKFLAGS ПОСЛЕ пользовательских флагов (C:\.platformio\platforms\ch32v\builder\frameworks\_bare.py,
  строки 64 и 87). В GCC побеждает последний `-O` → пользовательский `-Oz` игнорируется.
  Для смены уровня нужен patch _bare.py (временно, с бэкапом) или post: extra_scripts —
  но post-скрипты бесполезны: SCons-клон окружения для сборки создаётся раньше их выполнения.
- `-Oz` (GCC 12.2) даёт байт-в-байт тот же код, что `-Os` на этой кодовой базе.
- `--gc-sections`, `-ffunction-sections`, `-msave-restore` уже включены платформой.
- PIO решает перекомпиляцию по **MD5 контента**, не по mtime: touch не помогает,
  комментарии в platformio.ini не триггерят пересборку. Изменения _bare.py тоже не
  отслеживаются — после правки обязателен `pio run -t clean`.
- pio run -v может падать с UnicodeEncodeError на кириллических путях (имя пользователя в PATH).

## Мёртвый код (нулевая цена сейчас)
`mini_printf.c` целиком (mini_printf, out_char, out_format, uart_init, ~10 call-сайтов out_char)
вырезается LTO, т.к. `UART_DEBUG`/`PRINT_ADC` в include/proteus_config.h закомментированы →
`EE_PRINTF()` пустой. Сейчас 0 байт; при включении отладки добавит ~600–1000 Б.

## Если понадобится реальная экономия Flash
Не атрибуты, а: фичи-флаги `USE_STROBE` / `USE_BLOCK` / `USE_BATTIND` / `USE_MENU`
(include/proteus_config.h) — сотни байт каждый. Запас базы: 192 Б. Шум от микроправок
сигнатур — ±36 Б (пример start_aux_cmd), не стоит внимания.

## Методика A/B замера (воспроизведение)
1. `git stash push -- <file>` (или патч файла) → `pio run` → записать Flash из вывода.
2. Для разбора: `firmware.lst` в .pio/build/genericCH32V003F4U6/ — парсинг строк
   `^[0-9a-f]{8} <имя>:` даёт размеры символов (расстояние до следующего символа);
   нормализовать суффиксы `.constprop.N/.isra.N/.part.N`.
3. Отсутствие символа функции в lst при живых вызовах = функция инлайнена во все вызовы.
4. После экспериментов вернуть всё: правки источников, platformio.ini, _bare.py (обязательно!).
