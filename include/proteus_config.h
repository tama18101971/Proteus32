#ifndef PROTEUS_CONFIG_H
#define PROTEUS_CONFIG_H

#include <stdint.h>

/*
 * proteus_config.h — Глобальные настройки проекта
 *
 * Тактирование:
 *   SYSCLK_HZ      — единственный источник частоты для всей периферии
 *   SPIN_LOOPS_MS  — калибровка программных задержек/таймаутов
 *
 * Управление отладочным выводом:
 *   UART_DEBUG    — включает UART отладку (mini_printf)
 *   PRINT_ADC      — включает вывод ADC-значений
 *
 * EE_PRINTF() — макрос отладочного вывода:
 *   - Если UART_DEBUG или PRINT_ADC определены → mini_printf()
 *   - Иначе → пустой макрос (нет вывода, нет потребления)
 */

/* ── Тактовая частота системы ─────────────────────────────────────
 * У CH32V003 нет делителей APB: регистр RCC_CFGR0 содержит только
 * HPRE / ADCPRE / SW / SWS / PLLSRC (RM V1.9, разд. 3.4.2), поэтому
 * HCLK = PCLK1 = PCLK2 = SYSCLK. Одно значение задаёт тактирование
 * ядра, TIM2, I2C1, USART1 и (через ADCPRE) АЦП.
 *
 * ВНИМАНИЕ: значение обязано совпадать с board_build.f_cpu в
 * platformio.ini. f_cpu выбирает, какую SetSysClockTo_*() соберёт SDK,
 * то есть ФАКТИЧЕСКУЮ частоту; SYSCLK_HZ задаёт делители периферии,
 * то есть ОЖИДАЕМУЮ. Расхождение компилятором не отлавливается —
 * результатом будут молча неверные тайминги (тик, SCL, скорость UART).
 */
#define SYSCLK_HZ               24000000u

/* Целое число МГц требуется для поля I2C1->CTLR2.FREQ[5:0]; оно же
 * гарантирует целость SYSCLK_HZ / (1000 * SPIN_CYCLES_PER_ITER). */
_Static_assert(SYSCLK_HZ % 1000000u == 0u,
               "SYSCLK_HZ must be a whole number of MHz");
/* Границы поля FREQ[5:0]: 001000b..110000b, то есть 8..48 МГц (RM 13.11.2).
 * Верхняя граница также не даёт превысить HCLK max 50 МГц (DS табл. 3-2). */
_Static_assert(SYSCLK_HZ >= 8000000u && SYSCLK_HZ <= 48000000u,
               "SYSCLK_HZ outside the I2C1 CTLR2.FREQ range (8..48 MHz)");

/* Калибровка программных задержек: плотный цикл по счётчику в стеке
 * (lw / addi / sw / bnez) — ориентировочно 4 такта ядра на итерацию.
 * Значение приблизительное: фактическое число тактов зависит и от
 * задержки выборки из Flash (FLASH_ACTLR.LATENCY), которая меняется
 * вместе с частотой. Годится только для пауз и таймаутов.
 *
 * НЕ применять к циклам вида `for (volatile int i = 0; i < N; i++)`:
 * у них другое тело (~6 тактов), их счётчики калибруются отдельно
 * по месту использования (см. adc.c). */
#define SPIN_CYCLES_PER_ITER    4u
#define SPIN_LOOPS_MS(ms) \
    ((uint32_t)(ms) * (SYSCLK_HZ / (1000u * SPIN_CYCLES_PER_ITER)))

// Раскомментировать для отладки через UART
//#define UART_DEBUG
//#define PRINT_ADC

/* Скорость отладочного UART. Должна совпадать с monitor_speed в
 * platformio.ini, иначе вывод в мониторе будет нечитаемым. */
#define UART_DEBUG_BAUD         460800u

/* Ограничение ожидания флага TC USART1 перед потерей его тактирования
 * (вход в STANDBY, ISR кнопки). Один байт при 460800 бод — ~22 мкс,
 * поэтому 1 мс даёт запас на порядок и при этом не даёт неисправному
 * либо незаклокированному USART подвесить обработчик. */
#define UART_TC_WAIT_LOOPS      SPIN_LOOPS_MS(1)

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
