#ifndef WL_FLASH_H
#define WL_FLASH_H

#include <stdint.h>
#include <stddef.h>

#define FLASH_KEY1          0x45670123UL   /* Первый ключ разблокировки flash-контроллера CH32V003 (обязательная последовательность записей) */
#define FLASH_KEY2          0xCDEF89ABUL   /* Второй ключ разблокировки flash-контроллера CH32V003 (обязательная последовательность записей) */

/* Тип-перечисление для статусов тестирования (0=ошибка, 1=успех) */
typedef enum
{
    FAILED = 0,  /* Статус неудачи — запись 0 */
    PASSED = !FAILED  /* Статус успеха — запись 1 (инверсия FAILED) */
} TestStatus_t;

// ============================================================================
// FLASH ADDRESSES — EEPROM region at the end of 16 KB flash on CH32V003
// ============================================================================
// CH32V003: 16 KB flash = 0x08000000 .. 0x08003FFF
// We reserve the last 256 bytes (4 pages of 64 bytes each) for EEPROM.
// The remaining 16000 bytes (0x08000000 .. 0x08003EFF) are for the program.
// This is configured in platformio.ini via --defsym=FLASH_SIZE=16000.
/* Адрес начала EEPROM-области в flash (последние 256 байт 16KB flash чипа CH32V003).
   Чип: 16KB flash = 0x08000000 .. 0x08003FFF, program code: 0x08000000 .. 0x08003EFF */
#define PAGE_WRITE_START_ADDR  ((uint32_t)0x08003F00)  /* Начало EEPROM-области (адрес 0x08003F00) */
/* Адрес конца EEPROM-области (эксклюзивный — запись не по этому адресу).
   EEPROM-область = 256 байт = 4 страницы по 64 байта */
#define PAGE_WRITE_END_ADDR    ((uint32_t)0x08004000)  /* Конец EEPROM-области (адрес НЕ включается в запись) */
#define FLASH_PAGE_SIZE        64                       /* Размер одной страницы flash в CH32V003 = 64 байта */
#define DATAWIDTH              4                        /* Размер слова для чтения/записи = 4 байта (uint32_t).
                                                          Все операции с flash идут словами по 4 байта */

// Get page start address for any address in EEPROM region
/* Макрос для получения адреса начала страницы по произвольному адресу.
   Очищает нижние 6 бит (64-1=63=0x3F) — округляет адрес вниз до ближайшего начала страницы.
   Используется для определения, какую страницу нужно стирать перед записью */
#define GET_PAGE_ADDR(addr)    ((addr) & ~(FLASH_PAGE_SIZE - 1))  /* Адрес начала страницы для данного addr */

// Number of data slot fields in the struct (user data)
#define SLOT_COUNT 10  /* Количество слотов данных в структуре (слоты хранят функции кнопок) */

// ============================================================================
// AUXILIARY STRUCTURES — packed bitfields (each occupies 1-3 bytes)
// ============================================================================

// Backlight and beacon flags (1 byte total: 1+1+1+2+3 = 8 bits)
/* Структура флагов подсветки и маяка (1 байт: 1+1+1+2+3 = 8 бит).
   Атрибут packed — убирает padding между полями, структура занимает ровно 1 байт.
   Используется для экономии места во flash (EEPROM-область очень мала — 256 байт) */
struct __attribute__((packed)) aux_flags {
    uint8_t bcklight_type  : 1;  /* Тип подсветки (0 или 1 — два варианта типа подсветки) */
    uint8_t bcklight_color : 1;  /* Цвет подсветки (0 или 1 — два варианта цвета, например красный/синий) */
    uint8_t beacon_on      : 1;  /* Маяк включен (0=выкл, 1=вкл) */
    uint8_t beacon_mode    : 2;  /* Частота маяка (3 возможных режима мигания) */
    uint8_t beacon_type    : 3;  /* Цвет маяка ( например: красный - 001 /синий - 010 /белый LED - 100) */
};

// Engineering flags (3 bytes total: 5+7+7+5 = 24 bits)
// Suppress GCC warning about signed/unsigned in packed bitfields
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked-bitfield-compat"

/* Структура инженерных флагов (3 байта: 5+7+7+5 = 24 бита).
   Хранит калибровочные поправки и минимальный уровень.
   Знаковые поля (int8_t) используют 2-е дополнение — отрицательные значения для уменьшения */
struct __attribute__((packed)) eng_flags {
    uint8_t min_level : 5;  /* Минимальный уровень яркости (0..31) — нижний предел регулировки */
    int8_t  vc        : 7;  /* Поправка напряжения (-64..+63) — компенсация отклонений АЦП напряжения батареи */
    int8_t  tc        : 7;  /* Поправка температуры (-64..+63) — компенсация отклонений датчика температуры */
    uint8_t reserved  : 5;  /* Зарезервировано (выравнивание до 3 байтов — 5+7+7+5=24 бита = 3 байта) */
};

#pragma GCC diagnostic pop

// User flags (1 byte total: 4+1+1+2 = 8 bits)
/* Структура пользовательских флагов (1 байт: 4+1+1+2 = 8 бит).
   Хранит пользовательские настройки интерфейса */
struct __attribute__((packed)) usr_flags {
    uint8_t t_calibr       : 4;  /* Калибровка температуры (0..15) — поправка отображения температуры на экране */
    uint8_t eaux_onoff     : 1;  /* Дополнительный экран вкл/выкл (0=выкл, 1=вкл — показывать ли доп. информацию) */
    uint8_t id_plus_sid    : 1;  /* Режим ID + SID (0=обычный, 1=показывать ID и SID одновременно) */
    uint8_t reserved       : 2;  /* Зарезервировано (выравнивание до полного байта — 4+1+1+2=8 бит = 1 байт) */
};

// UI flags (1 byte total: 2+1+1+4 = 8 bits)
/* Структура флагов интерфейса (1 байт: 2+1+1+4 = 8 бит).
   Хранит настройки отображения UI */
struct __attribute__((packed)) ui_flags {
    uint8_t ui_index       : 2;  /* Индекс интерфейса (0..3 — 4 варианта отображения информации на экране) */
    uint8_t off_bckl       : 1;  /* Подсветка выключена (0=вкл, 1=выкл — отключить подсветку экрана) */
    uint8_t use_memory     : 1;  /* Использовать память (0=не использовать, 1=использовать запомненные настройки) */
    uint8_t reserved       : 4;  /* Зарезервировано (выравнивание до полного байта — 2+1+1+4=8 бит = 1 байт) */
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
/* Основная структура EEPROM-данных — ровно 32 байта (8 слов по 4 байта).
   Важно: sizeof(struct ee_data) ОБЯЗАН быть кратным 4 (DATAWIDTH).
   Причина: операции чтения/записи flash идут словами по 4 байта.
   Если sizeof не кратен 4 — последний байт CRC не будет записан/прочитан, и проверка CRC всегда упадёт.
   Поля упорядочены по offset'ам для выравнивания по 4-байтовым границам.
   Пacked + aligned(4) гарантирует: 1) нет padding-байтов между полями, 2) структура выровнена по 4 байта */
struct __attribute__((packed, aligned(4))) ee_data {
    uint32_t seq;               /* Номер последовательности (4 байта, offset 0): определяет последнюю запись.
                                   При каждой записи seq увеличивается на 1. При поиске валидной записи
                                   выбирается запись с наибольшим seq. Это позволяет перезаписывать данные
                                   без стирания flash — старые записи игнорируются, а новые — с бо́льшим seq */
    uint8_t version;            /* Версия структуры (1 байт, offset 4): для совместимости с прошивкой.
                                   ВНИМАНИЕ: на CH32V003 GCC packed uint8_t поля ломаются — используйте
                                   raw byte access: ((uint8_t*)&ee)[sizeof(uint32_t)] = value */

    uint16_t defauil_ui1;       /* Значение UI по умолчанию, часть 1 (2 байта, offset 5) */

    struct ui_flags  uif;       /* Флаги интерфейса (1 байт, offset 7): индекс UI, подсветка, память */
    struct aux_flags aux;       /* Флаги подсветки и маяка (1 байт, offset 8): тип/цвет подсветки, режим/тип маяка */
    struct eng_flags eng;       /* Инженерные настройки (3 байта, offset 9): калибровка напряжения/температуры, мин. уровень */
    struct usr_flags usr;       /* Пользовательские настройки (1 байт, offset 12): калибровка температуры, доп. экран */

    uint8_t defauil_ui2;        /* Значение UI по умолчанию, часть 2 (1 байт, offset 13) */
    uint8_t defauil_ui3;        /* Значение UI по умолчанию, часть 3 (1 байт, offset 14) */

    uint8_t slots[SLOT_COUNT];  /* Массив слотов данных (10 байтов, offset 15): хранит функции кнопок
                                   (1 клик, 2 клика, удержание и т.д.). Каждый слот — тип функции
                                   (например: индикатор батареи, маяк, стробоскоп, тактический режим) */

    uint8_t reserved[3];        /* Зарезервировано (3 байта, offset 25): выравнивание до 32 байтов.
                                   Без этого поле crc было бы на offset 25 (не кратно 4), что сломает запись flash */

    uint32_t crc;               /* CRC32 (4 байта, offset 28): контрольная сумма предыдущих 28 байтов (seq..reserved).
                                   Вычисляется по всем полям КРОМЕ crc. При чтении проверяется:
                                   crc32_calc(28, &data) == data.crc. Если не совпадает — запись повреждена */
};

extern struct ee_data ee;       /* Глобальный экземпляр структуры в RAM — актуальные данные EEPROM.
                                   Все модули программы читают/пишут настройки через эту переменную.
                                   При загрузке: read_ee() заполняет ee из flash.
                                   При сохранении: write_ee() записывает ee во flash */

// ============================================================================
// API
// ============================================================================
/* Инициализация структуры ee значениями по умолчанию.
   Вызывается при первом запуске или когда EEPROM пуст/повреждён.
   Заполняет все поля: версию, настройки подсветки, маяка, слоты кнопок и т.д. */
void init_ee(void);

/* Чтение последней записи из flash в структуру ee.
   Алгоритм: ищет все валидные записи (с проверкой CRC), выбирает запись с максимальным seq.
   Возвращает: 1=успех (данные прочитаны), 0=EEPROM пуст или ошибка CRC/версии */
int read_ee(void);

/* Запись текущей структуры ee во flash.
   Алгоритм: вычисляет CRC, находит свободный слот, стирает страницу если нужно, пишет данные.
   Если вся EEPROM-область заполнена — стирает все страницы и начинает с начала.
   Возвращает: 1=успех, 0=ошибка (не должна возникать при правильной работе) */
int write_ee(void);

void hanlde_ee(void);

#endif // WL_FLASH_H
