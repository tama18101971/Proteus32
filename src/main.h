#ifndef MAIN_H
#define MAIN_H

// Uncomment the line below to enable debug logs via UART
#define WL_FLASH_DEBUG 

#ifdef WL_FLASH_DEBUG
    #include <stdio.h>
    #define EE_PRINTF(...) printf(__VA_ARGS__)
#else
    #define EE_PRINTF(...) ((void)0)
#endif

#endif // MAIN_H