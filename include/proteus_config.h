#ifndef PROTEUS_CONFIG_H
#define PROTEUS_CONFIG_H

/*
 * proteus_config.h — Глобальные настройки проекта
 *
 * Управление отладочным выводом:
 *   WL_FLASH_DEBUG — включает UART отладку (mini_printf)
 *   PRINT_ADC      — включает вывод ADC-значений
 *
 * EE_PRINTF() — макрос отладочного вывода:
 *   - Если WL_FLASH_DEBUG или PRINT_ADC определены → mini_printf()
 *   - Иначе → пустой макрос (нет вывода, нет потребления)
 */

// Раскомментировать для отладки через UART
#define WL_FLASH_DEBUG
//#define PRINT_ADC

#define USE_BATTIND
//#define USE_EX_AUX
#define USE_STROBE
#define USE_BLOCK
#define USE_MENU
#define USE_MAGIC_SLOTS

#if defined(WL_FLASH_DEBUG) || defined(PRINT_ADC)
    #include "mini_printf.h"
    #define EE_PRINTF(...) mini_printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

#endif // PROTEUS_CONFIG_H
