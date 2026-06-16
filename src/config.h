#ifndef CONFIG_H
#define CONFIG_H

// Раскомментировать для отладки через UART
#define WL_FLASH_DEBUG

#ifdef WL_FLASH_DEBUG 
    #include "mini_printf.h"
    #define EE_PRINTF(...) mini_printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

//#define SLEEP_STANDBY
//#define PRINT_ADC

#endif // CONFIG_H
