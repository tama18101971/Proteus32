#ifndef PROTEUS_CONFIG_H
#define PROTEUS_CONFIG_H

/*
 * proteus_config.h — Глобальные настройки проекта
 *
 * Управление отладочным выводом:
 *   UART_DEBUG    — включает UART отладку (mini_printf)
 *   PRINT_ADC      — включает вывод ADC-значений
 *
 * EE_PRINTF() — макрос отладочного вывода:
 *   - Если UART_DEBUG или PRINT_ADC определены → mini_printf()
 *   - Иначе → пустой макрос (нет вывода, нет потребления)
 */

// Раскомментировать для отладки через UART
//#define UART_DEBUG
//#define PRINT_ADC

/* ── Флаги функций (каждый #define добавляет код в прошивку) ──────
 * Отключение лишних флагов экономит Flash (лимит 16128 байт).      */

#define USE_BATTIND         // Индикация уровня заряда батареи вспышками AUX-светодиодов (slot_batt_ind, aux_leds.c)
#define USE_AUX_ERR         // Индикация ошибки — три короткие красные вспышки (aux_err, aux_leds.c)
#define USE_EX_AUX          // Расширенные AUX-режимы: предупреждение о низком напряжении (start_exaux_voltage) и доп. команды в UI
#define USE_STROBE          // Стробоскоп: режим MODE_STROB, профили strobe_sos[], переключение up_strobe (ui.c)
#define USE_BLOCK           // Блокировка/лок-аут: MODE_BLOCK/MODE_BLOCK0, временный свет CMODE_MIN, разблокировка (ui.c)
#define USE_MENU            // Инженерное и пользовательское меню (вход с кнопкой при включении, ui.c)
#define USE_MAGIC_SLOTS     // «Волшебные» слоты: до 31 клика с маской длинного нажатия (ui_table.c, ui.c)

#if defined(UART_DEBUG) || defined(PRINT_ADC)
    #include "mini_printf.h"
    #define EE_PRINTF(...) mini_printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

#endif // PROTEUS_CONFIG_H
