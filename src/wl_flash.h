#ifndef WL_FLASH_H
#define WL_FLASH_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    FAILED = 0,
    PASSED = !FAILED
} TestStatus_t;

#define PAGE_WRITE_START_ADDR  ((uint32_t)0x08003F80)
#define PAGE_WRITE_END_ADDR    ((uint32_t)0x08004000) 
#define FLASH_PAGE_SIZE        64
#define DATAWIDTH              4  

#define SLOT_COUNT 10

// Занимает ровно 1 байт (сумма 8 бит)
struct __attribute__((packed)) aux_flags {
    uint8_t bcklight_type  : 1; // 0-й бит
    uint8_t bcklight_color : 1; // 1-й бит
    uint8_t beacon_on      : 1; // 2-й бит
    uint8_t beacon_mode    : 2; // 3..4 биты
    uint8_t beacon_type    : 3; // 5..7 биты
};

// Занимает ровно 3 байта (5 + 7 + 7 + 5 = 24 бита)
// Отключаем навязчивое уведомление о старых версиях GCC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked-bitfield-compat"

struct __attribute__((packed)) eng_flags {
    uint8_t min_level : 5;  
    int8_t  vc        : 7;  
    int8_t  tc        : 7;  
    uint8_t reserved  : 5;  
};

#pragma GCC diagnostic pop // Возвращаем настройки компилятора обратно

// Занимает ровно 1 байт (сумма 6 бит, 2 бита свободны)
struct __attribute__((packed)) usr_flags {
    uint8_t t_calibr       : 4; 
    uint8_t eaux_onoff     : 1; 
    uint8_t id_plus_sid    : 1;
    uint8_t reserved       : 2; // Добивка до полного байта
};

// Занимает ровно 1 байт (сумма 4 бита, 4 бита свободны)
struct __attribute__((packed)) ui_flags {
    uint8_t ui_index       : 2; 
    uint8_t off_bckl       : 1; 
    uint8_t use_memory     : 1;
    uint8_t reserved       : 4; // Добивка до полного байта
};

// Главная структура
struct __attribute__((packed, aligned(4))) ee_data {
    uint32_t counter;           // 4 байта (ставим 32-битные поля первыми)
    
    uint16_t defauil_ui1;       // 2 байта
    
    struct ui_flags  uif;       // 1 байт
    struct aux_flags aux;       // 1 байт
    struct eng_flags eng;       // 3 байта
    struct usr_flags usr;       // 1 byte
    
    uint8_t defauil_ui2;        // 1 байт
    uint8_t defauil_ui3;        // 1 байт
    
    uint8_t slots[SLOT_COUNT];  // 10 байт
    
    uint8_t reserved_align;     // 1 байт (!!!) ВАЖНО: Добиваем структуру до кратности 4 байтам
    
    uint32_t crc;               // 4 байта
};

extern struct ee_data ee;				// всего 28 байт

void init_ee(void);
int read_ee(void);
int write_ee(void);

#endif // WL_FLASH_H