#ifndef WL_FLASH_H
#define WL_FLASH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FLASH_KEY1          0x45670123UL   
#define FLASH_KEY2          0xCDEF89ABUL   

#define PAGE_WRITE_START_ADDR  ((uint32_t)0x08003F00)  
#define PAGE_WRITE_END_ADDR    ((uint32_t)0x08004000)  
#define FLASH_PAGE_SIZE        64                       
#define DATAWIDTH              4                        

#define GET_PAGE_ADDR(addr)    ((addr) & ~(FLASH_PAGE_SIZE - 1))  
#define SLOT_COUNT 10  

#define FAILED 0
#define SUCCESS 1

// Состояния слота памяти
typedef enum {
    SLOT_STATE_EMPTY = 0, // Слот чистый (все 0xFF)
    SLOT_VALID,           // Данные целы, CRC32 совпадает
    SLOT_CORRUPTED        // Слот содержит данные, но CRC32 нарушен
} slot_state_t;

// Вспомогательные структуры битовых полей
struct __attribute__((packed)) aux_flags {
    uint8_t bcklight_type  : 1;  
    uint8_t bcklight_color : 1;  
    uint8_t beacon_on      : 1;  
    uint8_t beacon_mode    : 2;  
    uint8_t beacon_type    : 3;  
};

struct __attribute__((packed)) eng_flags {
    uint8_t min_level : 5;  
    int8_t  vc        : 7;  
    int8_t  tc        : 7;  
    uint8_t reserved  : 5;  
};

struct __attribute__((packed)) usr_flags {
    uint8_t t_calibr       : 4;  
    uint8_t eaux_onoff     : 1;  
    uint8_t id_plus_sid    : 1;  
    uint8_t reserved       : 2;  
};

struct __attribute__((packed)) ui_flags {
    uint8_t ui_index       : 2;  
    uint8_t off_bckl       : 1;  
    uint8_t use_memory     : 1;  
    uint8_t reserved       : 4;  
};

// Основная структура (ровно 32 байта)
struct __attribute__((packed, aligned(4))) ee_data {
    uint32_t seq;               
    uint8_t version;            
    uint16_t default__ui1;       
    struct ui_flags  uif;       
    struct aux_flags aux;       
    struct eng_flags eng;       
    struct usr_flags usr;       
    uint8_t default__ui2;        
    uint8_t default__ui3;        
    uint8_t slots[SLOT_COUNT];  
    uint8_t reserved[3];        
    uint32_t crc;               
};

// Проверки времени компиляции размеров и выравнивания structures
_Static_assert(sizeof(struct ee_data) == 32, "Размер структуры ee_data должен быть строго 32 байта!");
_Static_assert(__alignof__(struct ee_data) == 4, "Структура ee_data должна быть выровнена по границе 4 байт!");

extern struct ee_data ee;       

#define EE_VERSION 1  

/* API */
void init_ee(void);
int read_ee(void);
int write_ee(void);
void handle_ee(void);

#endif // WL_FLASH_H