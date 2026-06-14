#include "wl_flash.h"
#include "ch32v00x.h"
#include <string.h>
#include "config.h"

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

struct ee_data ee;              // Global settings struct in RAM
static uint32_t ResAddress;     // Address where the next data chunk will be written
static uint32_t MaxSeq;         // Maximum sequence number among written slots

// ============================================================================
// EXTERNAL CH32V003 HAL FUNCTIONS
// ============================================================================
extern void FLASH_Unlock(void);
extern void FLASH_Lock(void);
extern FLASH_Status FLASH_ErasePage(uint32_t Page_Address);
extern FLASH_Status FLASH_ProgramWord(uint32_t Address, uint32_t Data);

// ============================================================================
// FLASH HELPERS
// ============================================================================
// CH32V003 STATR register flags:
//   Bit 6: BSY     — Busy (flash operation in progress)
//   Bit 5: EOP     — End Of Program (operation completed)
//   Bit 4: WRPRTERR — write protection error
//   Bit 2: PGERR   — programming error
// Flags are cleared by writing 1 (write-1-to-clear).
// ============================================================================

static inline void flash_clear_flags(void) {
    FLASH->STATR = FLASH_STATR_EOP | 0x00000010 | 0x00000004;
}

static inline void flash_wait_bsy(void) {
    while (FLASH->STATR & FLASH_STATR_BSY);
}

// ============================================================================
// CRC32 — nibble-based lookup table (16 entries, 64 bytes)
// ============================================================================
// Polynomial: 0x04C11DB7 (standard CRC-32)
// Initial value: 0xFFFFFFFF
// Processes 4 bits at a time (2 lookups per byte) — compact and fast.
// ============================================================================

#define EE_STRUCT_SIZE  sizeof(struct ee_data)
#define EE_DATA_WORDS   (EE_STRUCT_SIZE / DATAWIDTH)

static const uint32_t crc32_table[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

static uint32_t crc32_calc(size_t len, const void *data) {
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        crc = (crc >> 4) ^ crc32_table[crc & 0x0F];
        crc = (crc >> 4) ^ crc32_table[crc & 0x0F];
    }
    return crc;
}

// ============================================================================
// FLASH READ/ERASE HELPERS
// ============================================================================

// Read ee_data from flash (memory-mapped) into RAM buffer
static void flash_read_struct(uint32_t addr, struct ee_data *dest) {
    memcpy(dest, (const void *)addr, EE_STRUCT_SIZE);
}

// Check if slot at addr is valid: not empty (0xFF) and CRC matches
static int is_slot_valid(uint32_t addr) {
    if (*(volatile uint8_t *)addr == 0xFF) return 0;
    struct ee_data temp = {0};
    flash_read_struct(addr, &temp);
    return crc32_calc(EE_STRUCT_SIZE - DATAWIDTH, &temp) == temp.crc;
}

// Erase a single flash page (64 bytes). Protected by __disable_irq().
static int erase_flash_page(uint32_t page_start) {
    EE_PRINTF("[EEPROM] Erasing page 0x%08lX\r\n", page_start);
    __disable_irq();
    FLASH_Unlock();
    flash_wait_bsy();
    flash_clear_flags();
    FLASH_ErasePage(page_start);
    flash_wait_bsy();
    FLASH_Lock();
    __enable_irq();
    return 1;
}

// Check if a page is erased (first byte == 0xFF)
static int is_page_erased(uint32_t page_start) {
    return (*(volatile uint8_t *)page_start == 0xFF);
}

// ============================================================================
// SLOT SEARCH (flash_search_address)
// ============================================================================
// Scans slots from StartAddress to PAGE_WRITE_END_ADDR.
// Returns:
//   - Address of the first free slot (for writing)
//   - 0 — all slots are occupied (erase needed)
//
// Side effects:
//   - *LastValidAddr = address of the last valid record (or 0 if none)
//   - MaxSeq = maximum sequence number among records
// ============================================================================
uint32_t flash_search_address(uint32_t StartAddress, uint32_t *LastValidAddr) {
    uint32_t current = StartAddress;
    *LastValidAddr = 0;
    MaxSeq = 0;

    while (current <= PAGE_WRITE_END_ADDR - EE_STRUCT_SIZE) {
        if (is_slot_valid(current)) {
            struct ee_data temp = {0};
            flash_read_struct(current, &temp);
            if (temp.seq >= MaxSeq) {
                MaxSeq = temp.seq;
                *LastValidAddr = current;
            }
            current += EE_STRUCT_SIZE;
        } else {
            return current;
        }
    }
    return 0;
}

// ============================================================================
// STRUCT VERSION
// ============================================================================
// Increment this when the struct layout changes to detect firmware incompatibility.
// ============================================================================
#define EE_VERSION 1

// ============================================================================
// INITIALIZE DEFAULT SETTINGS
// ============================================================================
// Fills the global struct ee with initial values.
// Called on first boot (flash empty), CRC error, or version mismatch.
// ============================================================================
void init_ee(void) {
    EE_PRINTF("[EEPROM] Init defaults\r\n");
    memset(&ee, 0, EE_STRUCT_SIZE);
    ((uint8_t *)&ee)[4] = EE_VERSION;

    ee.eng.min_level = 1;
    ee.uif.ui_index = 1;
    ee.aux.bcklight_color = 1;
    ee.aux.bcklight_type = 1;
    ee.aux.beacon_mode = 1;
    ee.aux.beacon_on = 1;
    ee.aux.beacon_type = 2;
    ee.usr.t_calibr = 6;
}

// ============================================================================
// READ SETTINGS FROM FLASH
// ============================================================================
// Algorithm:
//   1. flash_search_address() finds the last valid record
//   2. If no records (last_data_address == 0) — return 0 (empty)
//   3. Copy data from flash, verify CRC32 (entire struct including crc)
//   4. Check version for firmware compatibility
//   5. Copy into global ee, return 1
//
// Returns: 1 = data read successfully, 0 = empty, CRC error, or version mismatch
// ============================================================================
int read_ee(void) {
    uint32_t last_data_address = 0;
    ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, &last_data_address);
    EE_PRINTF("[EEPROM] Write ptr: 0x%08lX, Last valid: 0x%08lX\r\n", ResAddress, last_data_address);

    if (last_data_address == 0) {
        EE_PRINTF("[EEPROM] EMPTY\r\n");
        return 0;
    }

    struct ee_data temp_ee = {0};
    flash_read_struct(last_data_address, &temp_ee);

    uint32_t sum = crc32_calc(EE_STRUCT_SIZE, &temp_ee);
    EE_PRINTF("[EEPROM] CRC: 0x%08lX\r\n", sum);

    if (sum != 0) {
        EE_PRINTF("[EEPROM] CRC ERROR\r\n");
        return 0;
    }

    uint8_t file_version = ((uint8_t *)&temp_ee)[4];
    if (file_version != EE_VERSION) {
        EE_PRINTF("[EEPROM] VER MISMATCH %u!=%u\r\n", file_version, EE_VERSION);
        return 0;
    }

    memcpy(&ee, &temp_ee, EE_STRUCT_SIZE);
    MaxSeq = temp_ee.seq;
    EE_PRINTF("[EEPROM] OK seq=%lu\r\n", ee.seq);
    return 1;
}

// ============================================================================
// WRITE SETTINGS TO FLASH
// ============================================================================
// Algorithm:
//   1. Set ee.seq = MaxSeq + 1, compute CRC32
//   2. If ResAddress == 0 (no slot found) — scan again
//   3. If still 0 (all slots full) — erase all 4 pages, reset
//   4. Otherwise, erase current page if needed (only for first slot)
//   5. Write struct word-by-word via FLASH_ProgramWord
//   6. On error — abort, previous records preserved
//
// Returns: 1 = write successful, 0 = error
// ============================================================================
int write_ee(void) {
    ee.seq = MaxSeq + 1;
    ee.crc = crc32_calc(EE_STRUCT_SIZE - DATAWIDTH, &ee);

    if (ResAddress == 0) {
        uint32_t dummy;
        ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, &dummy);
    }

    if (ResAddress == 0) {
        // All slots occupied — erase all pages and write to the beginning
        for (uint32_t p = PAGE_WRITE_START_ADDR; p < PAGE_WRITE_END_ADDR; p += FLASH_PAGE_SIZE) {
            erase_flash_page(p);
        }
        MaxSeq = 0;
        ResAddress = PAGE_WRITE_START_ADDR;
    } else {
        // Erase page only when writing to the FIRST slot (address is page-aligned)
        // This prevents destroying data in other slots of the same page
        uint32_t page_start = GET_PAGE_ADDR(ResAddress);
        if ((ResAddress == page_start) && !is_page_erased(page_start)) {
            erase_flash_page(page_start);
        }
    }

    __disable_irq();
    FLASH_Unlock();
    flash_wait_bsy();
    flash_clear_flags();

    uint32_t *p = (uint32_t *)&ee;
    EE_PRINTF("[EEPROM] Write 0x%08lX seq=%lu\r\n", ResAddress, ee.seq);

    for (uint32_t i = 0; i < EE_DATA_WORDS; i++) {
        if (FLASH_ProgramWord(ResAddress, p[i]) != FLASH_COMPLETE) {
            EE_PRINTF("[EEPROM] WRITE ERR\r\n");
            FLASH_Lock();
            __enable_irq();
            return 0;
        }
        ResAddress += DATAWIDTH;
    }

    flash_wait_bsy();
    FLASH_Lock();
    __enable_irq();
    MaxSeq = ee.seq;
    return 1;
}
