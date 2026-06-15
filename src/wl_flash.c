#include "wl_flash.h"
#include "ch32v00x.h"
#include <string.h>
#include "aux_leds.h"
#include "array.h"
#include "main.h"
#include "button.h"

/* ============================================================
 * GLOBAL VARIABLES
 * ============================================================ */
struct ee_data ee;
static uint32_t ResAddress;
static uint32_t MaxSeq;

/* ============================================================
 * FLASH helpers (registers only)
 * ============================================================ */
static inline void flash_wait_bsy(void)
{
    while (FLASH->STATR & FLASH_STATR_BSY);
}

static inline void flash_clear_flags(void)
{
    FLASH->STATR = FLASH_STATR_EOP | 0x10 | 0x04;
}

/* ============================================================
 * CRC32 (nibble-based, 16 entries)
 * ============================================================ */
#define EE_STRUCT_SIZE  sizeof(struct ee_data)
#define EE_DATA_WORDS   (EE_STRUCT_SIZE / DATAWIDTH)

static const uint32_t crc32_table[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
};

static uint32_t crc32_calc(uint32_t len, const void *data)
{
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        crc = (crc >> 4) ^ crc32_table[crc & 0x0F];
        crc = (crc >> 4) ^ crc32_table[crc & 0x0F];
    }
    return crc;
}

/* ============================================================
 * FLASH READ / VALIDATE
 * ============================================================ */
static void flash_read_struct(uint32_t addr, struct ee_data *dest)
{
    memcpy(dest, (const void *)addr, EE_STRUCT_SIZE);
}

static int is_slot_valid(uint32_t addr)
{
    if (*(volatile uint8_t *)addr == 0xFF) return 0;
    struct ee_data temp = {0};
    flash_read_struct(addr, &temp);
    return crc32_calc(EE_STRUCT_SIZE - DATAWIDTH, &temp) == temp.crc;
}

/* ============================================================
 * FLASH ERASE (1KB page via PER register)
 * ============================================================ */
static int erase_flash_page(uint32_t page_start)
{
    EE_PRINTF("[EEPROM] Erasing page 0x%08lX\r\n", page_start);
    __disable_irq();

    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    flash_wait_bsy();
    flash_clear_flags();

    FLASH->CTLR |= FLASH_CTLR_PER;
    FLASH->ADDR = page_start;
    FLASH->CTLR |= FLASH_CTLR_STRT;
    flash_wait_bsy();
    FLASH->CTLR &= ~FLASH_CTLR_PER;

    FLASH->CTLR |= FLASH_CTLR_LOCK;
    __enable_irq();
    return 1;
}

static int is_page_erased(uint32_t page_start)
{
    return (*(volatile uint8_t *)page_start == 0xFF);
}

/* ============================================================
 * SLOT SEARCH
 * ============================================================ */
uint32_t flash_search_address(uint32_t StartAddress, uint32_t *LastValidAddr)
{
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

/* ============================================================
 * STRUCT VERSION
 * ============================================================ */
#define EE_VERSION 1

/* ============================================================
 * READ FROM FLASH
 * ============================================================ */
int read_ee(void)
{
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

/* ============================================================
 * WRITE TO FLASH
 * ============================================================ */
int write_ee(void)
{
    ee.seq = MaxSeq + 1;
    ee.crc = crc32_calc(EE_STRUCT_SIZE - DATAWIDTH, &ee);

    if (ResAddress == 0) {
        uint32_t dummy;
        ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, &dummy);
    }

    if (ResAddress == 0) {
        for (uint32_t p = PAGE_WRITE_START_ADDR; p < PAGE_WRITE_END_ADDR; p += FLASH_PAGE_SIZE) {
            erase_flash_page(p);
        }
        MaxSeq = 0;
        ResAddress = PAGE_WRITE_START_ADDR;
    } else {
        uint32_t page_start = GET_PAGE_ADDR(ResAddress);
        if ((ResAddress == page_start) && !is_page_erased(page_start)) {
            erase_flash_page(page_start);
        }
    }

    __disable_irq();

    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    flash_wait_bsy();
    flash_clear_flags();

    uint32_t *p = (uint32_t *)&ee;
    EE_PRINTF("[EEPROM] Write 0x%08lX seq=%lu\r\n", ResAddress, ee.seq);

    for (uint32_t i = 0; i < EE_DATA_WORDS; i++) {
        FLASH->CTLR |= FLASH_CTLR_PG;
        *(volatile uint16_t *)ResAddress = (uint16_t)p[i];
        flash_wait_bsy();
        *(volatile uint16_t *)(ResAddress + 2) = (uint16_t)(p[i] >> 16);
        flash_wait_bsy();
        FLASH->CTLR &= ~FLASH_CTLR_PG;
        ResAddress += DATAWIDTH;
    }

    flash_wait_bsy();
    FLASH->CTLR |= FLASH_CTLR_LOCK;
    __enable_irq();
    MaxSeq = ee.seq;
    return 1;
}

/* ============================================================
 * INITIALIZE DEFAULTS
 * ============================================================ */
void init_ee(void)
{
    EE_PRINTF("[EEPROM] Init defaults\r\n");
    memset(&ee, 0, EE_STRUCT_SIZE);
    ((uint8_t *)&ee)[4] = EE_VERSION;

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

