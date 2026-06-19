#ifndef CONFIG_H
#define CONFIG_H

// Раскомментировать для отладки через UART
#define WL_FLASH_DEBUG
//#define PRINT_ADC

#if defined(WL_FLASH_DEBUG) || defined(PRINT_ADC)
    #include "mini_printf.h"
    #define EE_PRINTF(...) mini_printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

#endif // CONFIG_H
