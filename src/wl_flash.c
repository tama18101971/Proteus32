#include "wl_flash.h"
#include "ch32v00x.h"
#include <string.h>
#include "aux_leds.h"
#include "array.h"
#include "main.h"

struct ee_data ee;
static uint32_t ResAddress; 

extern void FLASH_Unlock(void);
extern void FLASH_Lock(void);
extern FLASH_Status FLASH_ErasePage(uint32_t Page_Address);
extern FLASH_Status FLASH_ProgramWord(uint32_t Address, uint32_t Data);

static inline void flash_clear_flags(void) {
    FLASH->STATR = FLASH_STATR_EOP | 0x00000010 | 0x00000004;
}

// CRC32 calculation with Little-Endian byte order in 32-bit Flash word
static uint32_t crc32_le_words(size_t len, const void *data) {
    const uint8_t *buffer = (const uint8_t*) data;
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i += 4) {
        for (int b = 3; b >= 0; b--) {
            crc ^= ((uint32_t)buffer[i + b] << 24);
            for (int bit = 0; bit < 8; bit++) {
                if (crc & (1UL << 31)) {
                    crc = (crc << 1) ^ 0x04C11DB7;
                } else {
                    crc = (crc << 1);
                }
            }
        }
    }
    return crc;
}

// Erase allocated Flash pages
static int erase_flash(void) {
    EE_PRINTF("[EEPROM] Sector full. Erasing flash pages for rollover...\r\n");
    FLASH_Unlock();
    flash_clear_flags();
    FLASH_ErasePage(PAGE_WRITE_START_ADDR);
    FLASH_ErasePage(PAGE_WRITE_START_ADDR + FLASH_PAGE_SIZE);
    FLASH_Lock();
    return 1;
}

// Search for the write address and determine the address of the last valid structure
uint32_t flash_search_address(uint32_t StartAddress, uint32_t BufferLength, uint32_t *LastValidAddr) {
    uint32_t current = StartAddress;
    uint32_t write_addr = 0;
    *LastValidAddr = 0; 

    while (current <= PAGE_WRITE_END_ADDR - BufferLength) {
        if (*(volatile uint8_t*)current != 0xFF) {
            *LastValidAddr = current; 
            current += BufferLength;  
        } else {
            write_addr = current;
            break;
        }
    }
    return write_addr; 
}

void init_ee(void) {
    EE_PRINTF("[EEPROM] Initialization with defaults.\r\n");
    ee.counter = 0;

	// eng menu
	ee.eng.min_level = 1;
	ee.eng.vc = 0;
	ee.eng.tc = 0;

	// select UI
	ee.uif.ui_index = 1;
	ee.uif.use_memory = 0;

	// bcklight
	ee.aux.bcklight_color = BCK_COLOR_RED;
	ee.aux.bcklight_type = BCK_TYPE_ON;

	// beacon
	ee.aux.beacon_mode = BCON_2S;
	ee.aux.beacon_on = true;
	ee.aux.beacon_type = RED_LED;

	// user menu
	ee.usr.eaux_onoff = false;
	ee.usr.id_plus_sid = false;
	ee.usr.t_calibr = 6;

	// defaults
	ee.defauil_ui1 = DEFAULT_STEP;
	ee.defauil_ui2 = DEFAULT_MODE;

	// slots
	ee.slots[0] = SLOT_BATT_IND;	// 1 кликa + удержание
	ee.slots[1] = SLOT_BEACON;		// 2 клика + удержание
	ee.slots[2] = SLOT_STROBE;		// 3 клика
	ee.slots[3] = SLOT_TACTICAL;	// 3 клика + удержание
	ee.slots[4] = SLOT_BLOCK;		// 4 клика
	ee.slots[5] = SLOT_EMPTY;		// 4 клика + удержание
	ee.slots[6] = SLOT_EMPTY;		// 5 клика
	ee.slots[7] = SLOT_MEMORY;		// 5 клика + удержание
	ee.slots[8] = SLOT_BACKLIGHT;	// 6 клика
	ee.slots[9] = SLOT_IPSMODE;		// 6 клика + удержание
}

// Read settings from Flash
int read_ee(void) {
    uint32_t struct_size = sizeof(struct ee_data);
    uint32_t last_data_address = 0;
    
    ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, struct_size, &last_data_address);
    EE_PRINTF("[EEPROM] Search pointer for write: 0x%08lX, Last valid data: 0x%08lX\r\n", ResAddress, last_data_address);

    if (last_data_address == 0) {
        EE_PRINTF("[EEPROM] Read: Storage is EMPTY.\r\n");
        return 0; 
    }

    struct ee_data temp_ee;
    uint32_t *dest_ptr = (uint32_t*)(void*)&temp_ee;
    volatile uint32_t *src_ptr = (volatile uint32_t*)last_data_address;
    
    for (size_t i = 0; i < struct_size / DATAWIDTH; i++) {
        dest_ptr[i] = src_ptr[i];
    }

    uint32_t sum = crc32_le_words(struct_size, &temp_ee);
    EE_PRINTF("[EEPROM] Calculating CRC... Result: 0x%08lX\r\n", sum);

    if (sum == 0) { 
        memcpy(&ee, &temp_ee, struct_size);
        EE_PRINTF("[EEPROM] Read: PASSED. Boot count: %lu\r\n", ee.counter);
        return 1;
    } else {
        EE_PRINTF("[EEPROM] Read: !CRC ERROR!\r\n");
        return 0;
    }
}

// Write settings to Flash
int write_ee(void) {
    uint32_t struct_size = sizeof(struct ee_data);
    uint32_t dummy_last_addr;

    ee.crc = crc32_le_words(struct_size - DATAWIDTH, &ee);

    ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, struct_size, &dummy_last_addr);

    if (ResAddress == 0) {
        erase_flash(); 
        ResAddress = PAGE_WRITE_START_ADDR; 
    }

    EE_PRINTF("[EEPROM] Writing to 0x%08lX. Counter = %lu, CRC = 0x%08lX\r\n", ResAddress, ee.counter, ee.crc);

    FLASH_Unlock();
    flash_clear_flags();

    uint32_t *pBuffer = (uint32_t*)(void*)&ee;
    int success = 1;

    for (uint32_t i = 0; i < struct_size / DATAWIDTH; i++) {
        if (FLASH_ProgramWord(ResAddress, pBuffer[i]) != FLASH_COMPLETE) {
            success = 0;
        }
        ResAddress = ResAddress + DATAWIDTH; 
    }

    FLASH_Lock();
    return success;
}