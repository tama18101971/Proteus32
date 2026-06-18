#ifndef CONFIG_H
#define CONFIG_H

// Раскомментировать для отладки через UART
//#define PRINT_ADC
//#define WL_FLASH_DEBUG

// раскоментировать для использования режима standby вместо Delay_Ms
//#define SLEEP_STANDBY


#if defined(WL_FLASH_DEBUG) || defined(PRINT_ADC)
    #include "mini_printf.h"
    #define EE_PRINTF(...) mini_printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

#endif // CONFIG_H
