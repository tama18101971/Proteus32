#ifndef WL_FLASH_H
#define WL_FLASH_H

#include <stdint.h>
#include <stddef.h>

#define FLASH_KEY1          0x45670123UL
#define FLASH_KEY2          0xCDEF89ABUL

typedef enum
{
    FAILED = 0,
    PASSED = !FAILED
} TestStatus_t;

// ============================================================================
// FLASH ADDRESSES — EEPROM region at the end of 16 KB flash on CH32V003
// ============================================================================
// CH32V003: 16 KB flash = 0x08000000 .. 0x08003FFF
// We reserve the last 256 bytes (4 pages of 64 bytes each) for EEPROM.
// The remaining 16000 bytes (0x08000000 .. 0x08003EFF) are for the program.
// This is configured in platformio.ini via --defsym=FLASH_SIZE=16000.
#define PAGE_WRITE_START_ADDR  ((uint32_t)0x08003F00)  // Start of EEPROM region
#define PAGE_WRITE_END_ADDR    ((uint32_t)0x08004000)  // End of EEPROM region (exclusive)
#define FLASH_PAGE_SIZE        64                       // CH32V003 flash page size = 64 bytes
#define DATAWIDTH              4                        // Read/write word size = 4 bytes (uint32_t)

// Get page start address for any address in EEPROM region
#define GET_PAGE_ADDR(addr)    ((addr) & ~(FLASH_PAGE_SIZE - 1))

// Number of data slot fields in the struct (user data)
#define SLOT_COUNT 10

// ============================================================================
// AUXILIARY STRUCTURES — packed bitfields (each occupies 1-3 bytes)
// ============================================================================

// Backlight and beacon flags (1 byte total: 1+1+1+2+3 = 8 bits)
struct __attribute__((packed)) aux_flags {
    uint8_t bcklight_type  : 1;  // Backlight type (0 or 1)
    uint8_t bcklight_color : 1;  // Backlight color (0 or 1)
    uint8_t beacon_on      : 1;  // Beacon enabled (0=off, 1=on)
    uint8_t beacon_mode    : 2;  // Beacon mode (0..3)
    uint8_t beacon_type    : 3;  // Beacon type (0..7)
};

// Engineering flags (3 bytes total: 5+7+7+5 = 24 bits)
// Suppress GCC warning about signed/unsigned in packed bitfields
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked-bitfield-compat"

struct __attribute__((packed)) eng_flags {
    uint8_t min_level : 5;  // Minimum level (0..31)
    int8_t  vc        : 7;  // Voltage correction (-64..+63)
    int8_t  tc        : 7;  // Temperature correction (-64..+63)
    uint8_t reserved  : 5;  // Reserved (padding to 3 bytes)
};

#pragma GCC diagnostic pop

// User flags (1 byte total: 4+1+1+2 = 8 bits)
struct __attribute__((packed)) usr_flags {
    uint8_t t_calibr       : 4;  // Temperature calibration (0..15)
    uint8_t eaux_onoff     : 1;  // Extra screen on/off
    uint8_t id_plus_sid    : 1;  // ID + SID mode
    uint8_t reserved       : 2;  // Reserved (padding to full byte)
};

// UI flags (1 byte total: 2+1+1+4 = 8 bits)
struct __attribute__((packed)) ui_flags {
    uint8_t ui_index       : 2;  // Interface index (0..3)
    uint8_t off_bckl       : 1;  // Backlight off
    uint8_t use_memory     : 1;  // Use memory
    uint8_t reserved       : 4;  // Reserved (padding to full byte)
};

// ============================================================================
// MAIN STRUCTURE ee_data — exactly 32 bytes
// ============================================================================
// IMPORTANT: sizeof(struct ee_data) MUST be a multiple of 4 (DATAWIDTH).
//   This is critical because read/write operates on 4-byte flash words.
//   If sizeof is not a multiple of 4, the last byte of CRC won't be
//   written/read and the CRC check will always fail.
//
//   Current layout (packed, aligned(4)):
//   offset  0: seq          (4 bytes) — sequence number (word-aligned)
//   offset  4: version      (1 byte)  — struct version for firmware compatibility
//                                   WARNING: packed uint8_t field access is broken
//                                   on CH32V003 GCC — use raw byte access instead.
//                                   See: ((uint8_t*)&ee)[sizeof(uint32_t)] = value
//   offset  5: defauil_ui1  (2 bytes) — default UI value
//   offset  7: uif          (1 byte)  — UI flags
//   offset  8: aux          (1 byte)  — backlight/beacon flags
//   offset  9: eng          (3 bytes) — engineering flags
//   offset 12: usr          (1 byte)  — user flags
//   offset 13: defauil_ui2  (1 byte)
//   offset 14: defauil_ui3  (1 byte)
//   offset 15: slots[10]   (10 bytes) — data slots
//   offset 25: reserved     (3 bytes) — padding to maintain 32-byte size
//   offset 28: crc          (4 bytes) — CRC32 of the preceding 28 bytes
//   = 32 bytes total
//
//   EXPANSION NOTES: when adding fields, recalculate offsets.
//   sizeof MUST remain a multiple of 4. If you need 33 bytes of data,
//   add 3 bytes of reserved (total 36), or remove unused fields.
// ============================================================================
struct __attribute__((packed, aligned(4))) ee_data {
    uint32_t seq;               // Sequence number: determines the latest record
    uint8_t version;            // Struct version (use raw byte access, see above)

    uint16_t defauil_ui1;       // Default UI value (part 1)

    struct ui_flags  uif;       // Interface flags
    struct aux_flags aux;       // Backlight and beacon flags
    struct eng_flags eng;       // Engineering settings
    struct usr_flags usr;       // User settings

    uint8_t defauil_ui2;        // Default UI value (part 2)
    uint8_t defauil_ui3;        // Default UI value (part 3)

    uint8_t slots[SLOT_COUNT];  // Data slot array (10 bytes)

    uint8_t reserved[3];        // Padding to maintain 32-byte size

    uint32_t crc;               // CRC32 of the preceding 28 bytes (seq..reserved)
};

extern struct ee_data ee;       // Global struct instance in RAM

// ============================================================================
// API
// ============================================================================
void init_ee(void);             // Initialize struct with default values
int read_ee(void);              // Read last record from flash. Returns: 1=OK, 0=empty/error
int write_ee(void);             // Write current struct to flash. Returns: 1=OK, 0=error

#endif // WL_FLASH_H
