// SPDX-License-Identifier: GPL-3.0-or-later

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
 * Отключение лишних флагов экономит Flash (лимит 16128 байт).
 * Дефолты = 1; для измерения вклада флага без правки исходников:
 *   PLATFORMIO_BUILD_FLAGS="-DUSE_X=0" pio run
 * Код использует #if USE_X, поэтому неопределённый макрос тоже
 * выключает функцию (как раньше #ifdef). */

#ifndef USE_IND_RGB
#define USE_IND_RGB 0       // RGB-индикатор: PC7=red, PC4=green, PC6=blue (вместо двухканального красного PC7+PC6). Сборка: PLATFORMIO_BUILD_FLAGS="-DUSE_IND_RGB=1" или env Proteus32-RGB
#endif
#ifndef USE_BATTIND
#define USE_BATTIND 1       // Индикация уровня заряда батареи вспышками AUX-светодиодов (slot_batt_ind, aux_leds.c)
#endif
#ifndef USE_AUX_ERR
#define USE_AUX_ERR 1       // Индикация ошибки/блокировки: aux_err + aux_dang (aux_leds.c); включается битом exaux (EE.uif) в меню пользователя
#endif
#ifndef USE_EX_AUX
#define USE_EX_AUX 1        // Расширенные AUX-режимы: предупреждение о низком напряжении (start_exaux_voltage) и доп. команды в UI
#endif
#if USE_IND_RGB
/* RGB-вариант: третий LED-канал + exaux вместе не помещаются во Flash
 * (лимит 16128 Б), поэтому в этом варианте драйвера расширенных AUX-режимов
 * нет вообще. Формат EE.aux тоже свой (app_data.h): старый здесь не
 * существует, поэтому WL_FORMAT_VERSION не меняется. */
#undef USE_EX_AUX
#define USE_EX_AUX 0
#endif
#ifndef USE_STROBE
#define USE_STROBE 1        // Стробоскоп: режим MODE_STROB, профили strobe_sos[], переключение up_strobe (ui.c)
#endif
#ifndef USE_BLOCK
#define USE_BLOCK 1         // Блокировка/лок-аут: MODE_BLOCK/MODE_BLOCK0, временный свет CMODE_MIN, разблокировка (ui.c)
#endif
#ifndef USE_MENU
#define USE_MENU 1          // Инженерное и пользовательское меню (вход с кнопкой при включении, ui.c)
#endif
#ifndef USE_MAGIC_SLOTS
#define USE_MAGIC_SLOTS 1   // «Волшебные» слоты: до 31 клика с маской длинного нажатия (ui_table.c, ui.c)
#endif
#ifndef USE_LSI_GUARD
/* Защита от отказа LSI/AWU: awu_ready + гейты термосна и старта AUX
 * (awu.c, main.c, aux_leds.c). По умолчанию ОТКЛЮЧЕНА: вероятность
 * отказа LSI оценена как очень низкая, а оба guard'а (LSI + строб)
 * вместе не помещаются в 16128 Б. Включение: PLATFORMIO_BUILD_FLAGS
 * "-DUSE_LSI_GUARD=1" (вклад +84 Б; при включённом строб-гейде
 * одновременно не помещается). Принимаемый риск при отключении:
 * без LSI строб не отсчитает первый OFF-полупериод и может залипнуть
 * на ON-яркости, термосон AWU_4S не разбудит MCU. */
#define USE_LSI_GUARD 1
#endif
#ifndef USE_STROBE_ADC_GUARD
#define USE_STROBE_ADC_GUARD 1  /* Прекращение строба при устойчивом отказе АЦП
 * (adc.c strobe_battery_state, main.c): 3 неудачных adc_read() подряд →
 * ВОССТАНОВИМЫЙ останов через off_strobe(). Подтверждённый разряд
 * (4 валидных отсчёта ниже 2.7 В) по-прежнему уводит в down_forever().
 * При 0 — прежнее поведение: неудачные выборки только рвут серию
 * подтверждений, строб может крутиться вслепую. */
#endif
#ifndef USE_BEACON_ADC_GUARD
#define USE_BEACON_ADC_GUARD 1  /* Прекращение маяка при устойчивом отказе АЦП
 * (aux_leds.c beacon_battery_check/start_aux_beacon, main.c): 3 неудачных
 * проверки АКБ подряд (по одной на цикл маяка) ставят RAM-защёлку и гасят
 * маяк до кнопочного пробуждения с валидной выборкой — иначе маяк
 * перезапускался бы каждым sleep-блоком и светил без защиты АКБ.
 * Подтверждённый разряд (2/3 отсчёта ниже 2.7 В) по-прежнему down_forever().
 * При 0 — прежнее поведение: маяк крутится без проверки АКБ. */
#endif

#if defined(UART_DEBUG) || defined(PRINT_ADC)
    #include "mini_printf.h"
    #define EE_PRINTF(...) mini_printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

#endif // PROTEUS_CONFIG_H
