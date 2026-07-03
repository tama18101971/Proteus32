/* ============================================================
 * wl_flash.c — EEPROM-эмуляция во flash для CH32V003
 * ============================================================ */
#include "wl_flash.h"
#include "ch32v00x.h"
#include <string.h>
#include "main.h"
#include "ui.h"

// Гарантируем наличие флага аппаратной ошибки выравнивания/программирования для CH32V003
// (Маска 0x20 соответствует 5-му биту регистра FLASH->STATR)
#ifndef FLASH_FLAG_PGAERR
#define FLASH_FLAG_PGAERR       ((uint32_t)0x00000020) 
#endif

/* ============================================================
 * GLOBAL VARIABLES
 * ============================================================ */

struct ee_data ee;                    

static uint32_t ResAddress; // Физический адрес для следующей записи
static uint32_t MaxSeq;     // Наибольший обнаруженный seq

#define EE_STRUCT_SIZE  sizeof(struct ee_data)   
#define TOTAL_SLOTS     ((PAGE_WRITE_END_ADDR - PAGE_WRITE_START_ADDR) / EE_STRUCT_SIZE) // 8 слотов

/* ============================================================
 * CRC32 (nibble-based, 16 entries)
 * ============================================================ */
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
 * FLASH helpers (Совместимые с ch32v00x_flash.h и защищенные)
 * ============================================================ */

/**
 * @brief  Ожидание освобождения контроллера Flash с таймаутом.
 * @return int : 1 — успешно освободился, 0 — аппаратный таймаут (ошибка)
 */
static inline int flash_wait_bsy(void)
{
    volatile uint32_t timeout = 2000000; 
    while (FLASH->STATR & FLASH_FLAG_BSY) {
        if (--timeout == 0) {
            return 0; // Аппаратный сбой: контроллер Flash заклинило
        }
    }
    return 1; 
}

static inline void flash_clear_flags(void)
{
    // Очищаем флаг завершения операции (EOP), защиту (WRPRTERR) и ошибку выравнивания (PGAERR)
    FLASH->STATR = FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR | FLASH_FLAG_PGAERR;  
}

static inline int flash_check_errors(void)
{
    // Проверяем только флаг реальной аппаратной ошибки — защита от записи (WRPRTERR).
    // ВАЖНО: бит 5 регистра STATR на CH32V00x — это EOP (успешное завершение операции),
    // а НЕ ошибка программирования. Проверка "& 0x00000020" ошибочно трактовала
    // штатное завершение записи как аппаратный сбой и прерывала write_ee()
    // практически сразу после первой удачно записанной половины слова —
    // именно это и было причиной "Hardware Flash Write Error!" в логе.
    if (FLASH->STATR & FLASH_FLAG_WRPRTERR) {
        flash_clear_flags(); // Сбрасываем флаги, чтобы разблокировать контроллер Flash
        return 1;            // Обнаружена аппаратная ошибка
    }
    return 0;                // Ошибок нет, всё чисто
}

static void flash_read_struct(uint32_t addr, struct ee_data *dest)
{
    memcpy(dest, (const void *)addr, EE_STRUCT_SIZE);  
}

// НИЗКОУРОВНЕВАЯ функция программирования 32 байт по произвольному адресу.
// Не трогает ee/ResAddress/MaxSeq — чистая операция "записать буфер по адресу".
// Используется и основным путём записи, и восстановлением слота при аномалии.
// Вызывающий должен сам делать верификацию после вызова, если она нужна.
static int flash_write_struct_raw(uint32_t addr, const struct ee_data *src)
{
    __disable_irq();

    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    flash_wait_bsy();

    const uint16_t *p16 = (const uint16_t *)src;
    uint32_t write_ptr = addr;
    int write_error = 0;

    for (uint32_t i = 0; i < (EE_STRUCT_SIZE / 2); i++)
    {
        flash_clear_flags();
        FLASH->CTLR |= FLASH_CTLR_PG;

        *(volatile uint16_t *)write_ptr = p16[i];
        flash_wait_bsy();

        FLASH->CTLR &= ~FLASH_CTLR_PG;

        if (flash_check_errors())
        {
            write_error = 1;
            break;
        }
        write_ptr += 2;
    }

    FLASH->CTLR |= FLASH_CTLR_LOCK;
    __enable_irq();

    return (write_error == 0);
}

static int is_page_erased(uint32_t page_start)
{
    volatile uint32_t *ptr = (volatile uint32_t *)page_start;
    for (uint32_t i = 0; i < (FLASH_PAGE_SIZE / 4); i++) {
        if (ptr[i] != 0xFFFFFFFF) {
            return 0; 
        }
    }
    return 1; 
}

/**
 * @brief  Определяет состояние слота и считывает данные, если они есть.
 * @param  addr: Физический адрес слота во Flash.
 * @param  out_data: Указатель на структуру для выгрузки считанных данных.
 * @return slot_state_t: Состояние слота (EMPTY, VALID, CORRUPTED).
 */
static slot_state_t get_slot_state(uint32_t addr, struct ee_data *out_data)
{
    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    
    // Быстрая проверка на пустоту по первому и последнему слову
    if (*ptr == 0xFFFFFFFF) {
        if (*(ptr + (EE_STRUCT_SIZE / 4) - 1) == 0xFFFFFFFF) {
            return SLOT_STATE_EMPTY;
        }
    }

    // Слот содержит данные — считываем их напрямую в целевой буфер
    flash_read_struct(addr, out_data);
    
    // Считаем CRC32 по вычитанным данным (размер структуры минус 4 байта поля crc)
    uint32_t calc_crc = crc32_calc(EE_STRUCT_SIZE - 4, out_data);
    
    if (calc_crc == out_data->crc) {
        return SLOT_VALID;
    }
    
    return SLOT_CORRUPTED;
}

/* ============================================================
 * SLOT SEARCH & PAGE ROTATION
 * ============================================================ */
uint32_t flash_search_address(uint32_t StartAddress, struct ee_data *out_valid_data)
{
    uint32_t best_valid_addr = 0;
    bool has_valid = false;
    MaxSeq = 0;                             

    // Сканируем абсолютно все 8 слотов памяти по всей кольцевой области
    for (uint32_t i = 0; i < TOTAL_SLOTS; i++) {
        uint32_t current_addr = StartAddress + (i * EE_STRUCT_SIZE);
        struct ee_data temp_scan; 
        
        // get_slot_state вычитает данные в temp_scan и проверит CRC за один проход
        if (get_slot_state(current_addr, &temp_scan) == SLOT_VALID) {
            if (!has_valid || temp_scan.seq >= MaxSeq) {
                MaxSeq = temp_scan.seq;
                best_valid_addr = current_addr;
                // Сохраняем только самый актуальный валидный слот
                memcpy(out_valid_data, &temp_scan, EE_STRUCT_SIZE);
                has_valid = true;
            }
        }
    }

    // Если валидных записей вообще нет (чистая память)
    if (!has_valid) {
        return StartAddress; 
    }

    // Логика Page Rotation: вычисляем точку следующей записи
    uint32_t next_addr = best_valid_addr + EE_STRUCT_SIZE;
    if (next_addr >= PAGE_WRITE_END_ADDR) {
        next_addr = StartAddress;
    }

    return next_addr;
}

/* ============================================================
 * FLASH ERASE
 * ============================================================ */
static int erase_flash_page(uint32_t page_start)
{
    EE_PRINTF("[EEPROM] Erasing page 0x%08lX\r\n", page_start);  
    __disable_irq();                        

    FLASH->KEYR = FLASH_KEY1;              
    FLASH->KEYR = FLASH_KEY2;              
    
    if (!flash_wait_bsy()) { __enable_irq(); return 0; } 
    flash_clear_flags();                   

    FLASH->CTLR |= FLASH_CTLR_PER;        
    FLASH->ADDR = page_start;              
    FLASH->CTLR |= FLASH_CTLR_STRT;       
    
    if (!flash_wait_bsy()) { 
        FLASH->CTLR &= ~FLASH_CTLR_PER;
        FLASH->CTLR |= FLASH_CTLR_LOCK;
        __enable_irq();
        return 0; 
    }                      
    
    FLASH->CTLR &= ~FLASH_CTLR_PER;       

    int err = flash_check_errors();
    FLASH->CTLR |= FLASH_CTLR_LOCK;       
    __enable_irq();                        
    
    return (err == 0);                              
}

/* ============================================================
 * READ FROM FLASH
 * ============================================================ */
int read_ee(void)
{
    struct ee_data temp_ee;           
    
    // Поиск активной точки. Контроль CRC32 уже встроен внутрь сканера
    ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, &temp_ee);  
    
    if (MaxSeq == 0) {           
        EE_PRINTF("[EEPROM] EMPTY OR ALL SLOTS CORRUPTED\r\n");    
        return 0;                           
    }

    EE_PRINTF("[EEPROM] Active slot found! Next Write Addr: 0x%08lX, MaxSeq: %lu\r\n", ResAddress, MaxSeq);  

    if (temp_ee.version != EE_VERSION) {       
        EE_PRINTF("[EEPROM] VERSION MISMATCH %u != %u\r\n", temp_ee.version, EE_VERSION);  
        return 0;                           
    }

    // CRC проверен, версия верна — копируем данные в рабочую переменную ee
    memcpy(&ee, &temp_ee, EE_STRUCT_SIZE);  
    return 1;                               
}

/* ============================================================
 * WRITE TO FLASH
 * ============================================================ */
int write_ee(void)
{
    ee.seq = MaxSeq + 1;                   
    ee.crc = crc32_calc(EE_STRUCT_SIZE - 4, &ee);  

if (ResAddress == 0) {                  
        struct ee_data dummy_buf; // <-- Меняем тип на struct ee_data
        ResAddress = flash_search_address(PAGE_WRITE_START_ADDR, &dummy_buf);  
    }
    
    uint32_t page_start = GET_PAGE_ADDR(ResAddress);

    // Учет кратности (2 записи на страницу) при циклическом журнале:
    if (ResAddress == page_start) {
        // Заходим на новую страницу в первый слот — обязательно стираем её
        if (!is_page_erased(page_start)) {
            if (!erase_flash_page(page_start)) {
                EE_PRINTF("[EEPROM] Page Erase Failed!\r\n");
                return 0;
            }
        }
    } else {
        // Мы пишем во ВТОРОЙ слот уже частично заполненной страницы.
        // Стирать её НЕЛЬЗЯ просто так, иначе сотрем первый слот.
        // Убеждаемся, что второй слот действительно пустой (0xFF) перед записью.
        struct ee_data dummy_buf;
        if (get_slot_state(ResAddress, &dummy_buf) != SLOT_STATE_EMPTY)
        {
            // АНОМАЛИЯ: второй слот забит мусором, а первый слот (page_start)
            // уже содержит АКТУАЛЬНУЮ валидную запись. Минимальная единица
            // стирания Flash — страница целиком, поэтому восстанавливаем:
            // сохраняем слот 0, стираем страницу, пишем слот 0 обратно,
            // и продолжаем запись во второй (теперь чистый) слот.
            EE_PRINTF("[EEPROM] Slot anomaly at 0x%08lX, recovering page 0x%08lX\r\n",
                      ResAddress, page_start);

            struct ee_data slot0_backup;
            flash_read_struct(page_start, &slot0_backup);

            uint32_t slot0_crc = crc32_calc(EE_STRUCT_SIZE - 4, &slot0_backup);
            int slot0_ok = (slot0_crc == slot0_backup.crc) && (slot0_backup.version == EE_VERSION);

            int recovered = 0;

            if (slot0_ok)
            {
                if (erase_flash_page(page_start))
                {
                    if (flash_write_struct_raw(page_start, &slot0_backup))
                    {
                        struct ee_data verify0;
                        flash_read_struct(page_start, &verify0);
                        if (memcmp(&slot0_backup, &verify0, EE_STRUCT_SIZE) == 0)
                        {
                            recovered = 1;
                            EE_PRINTF("[EEPROM] Page 0x%08lX recovered, slot0 restored\r\n", page_start);
                        }
                    }
                }
            }

            if (!recovered)
            {
                EE_PRINTF("[EEPROM] Recovery failed, falling back to next page\r\n");
                ResAddress = page_start + FLASH_PAGE_SIZE;
                if (ResAddress >= PAGE_WRITE_END_ADDR)
                {
                    ResAddress = PAGE_WRITE_START_ADDR;
                }
                page_start = GET_PAGE_ADDR(ResAddress);

                if (!erase_flash_page(page_start))
                    return 0;
            }
        }
    }

EE_PRINTF("[EEPROM] Writing to 0x%08lX seq=%lu\r\n", ResAddress, ee.seq);

    if (!flash_write_struct_raw(ResAddress, &ee))
    {
        EE_PRINTF("[EEPROM] Hardware Flash Write Error!\r\n");
        return 0;
    }

    // ВЕРИФИКАЦИЯ: Чтение записанного обратно и жесткое побитовое сравнение с RAM
    struct ee_data verify_buf;
    flash_read_struct(ResAddress, &verify_buf);
    if (memcmp(&ee, &verify_buf, EE_STRUCT_SIZE) != 0) {
        EE_PRINTF("[EEPROM] Verification FAILED after write!\r\n");
        return 0; 
    }

    // Инкремент адреса под будущие операции
    MaxSeq = ee.seq;
    ResAddress += EE_STRUCT_SIZE;

    if (ResAddress >= PAGE_WRITE_END_ADDR) {
        ResAddress = PAGE_WRITE_START_ADDR;
    }

    EE_PRINTF("[EEPROM] Write & Verify Successful\r\n");
    return 1;                              
}

/* ============================================================
 * INITIALIZE DEFAULTS
 * ============================================================ */
/* Инициализация структуры ee значениями по умолчанию.
   Вызывается при первом запуске (EEPROM пуста) или при сбросе настроек.
   Обнуляет структуру, устанавливает версию и вызывает ee_defaults() для заполнения полей.
   После вызова этой функции обычно вызывают write_ee() для сохранения во flash */
void init_ee(void)
{
    EE_PRINTF("[EEPROM] Init defaults\r\n");
    memset(&ee, 0, EE_STRUCT_SIZE);
    ((uint8_t *)&ee)[4] = EE_VERSION;
    ee_defaults(&ee);
}

void handle_ee(void) 
{
    if (need_save) {
        if (write_ee()) {
            need_save = 0; 
        }
    }
}