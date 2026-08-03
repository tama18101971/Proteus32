/**
 * @file ob.c
 * @brief Реализация модуля работы с Option Bytes контроллера CH32V003
 *
 * Этот модуль реализует хранение пользовательских данных в области
 * Option Bytes (OB) микроконтроллера CH32V003F4P6.
 *
 * Принцип работы:
 * - При запуске данные считываются из flash в кэш (_refresh)
 * - Геттеры/сеттеры работают только с кэшем в ОЗУ
 * - ob_Flush() записывает кэш обратно в flash и верифицирует
 *
 * Механизм записи option bytes на CH32V003:
 * 1. Разблокировка flash-контроллера (FLASH_KEY1 + FLASH_KEY2)
 * 2. Разблокировка записи option bytes (OBKEYR)
 * 3. Стирание option bytes (OPTER)
 * 4. Запись по словам (16 байт) с контролем готовности (BSY)
 * 5. Блокировка flash-контроллера
 *
 * Адресное пространство:
 * - OB_BASE (0x1FFFF800): стандартные регистры (12 байт)
 * - OB_ADDR (OB_BASE + 16): пользовательские данные (28 байт)
 */

#include <stdint.h>
#include <stdbool.h>
#include "ob.h"
#include "ch32v00x_flash.h"
#include "array.h"

uint8_t change = false;

/* Ключи разблокировки flash-контроллера (из datasheet CH32V003) */
#define FLASH_KEY1      0x45670123u
#define FLASH_KEY2      0xCDEF89ABu

/* Коды статуса операций с flash */
typedef uint8_t flash_status_t;
#define FLASH_OK        0   /* Операция выполнена успешно */
#define FLASH_TIMEOUT   1   /* Превышено время ожидания */

/*
 * Базовый адрес пользовательских данных.
 * Стандартные option bytes занимают 12 байт (OB_BASE..OB_BASE+11).
 * Пользовательские данные начинаются сразу после них.
 */
#define OB_ADDR     (OB_BASE + 16)

/* Маркер валидности: записывается в поле magic для определения
 * первого запуска или повреждения данных */
#define MAGIC           0xA5

/**
 * @brief Кэш пользовательских данных Option Bytes
 *
 * Структура хранит копию данных из flash. Все обращения
 * к данным идут через этот кэш, а запись в flash — только
 * при вызове ob_Flush().
 *
 * Раскладка в адресном пространстве (каждое поле = 2 байта адреса):
 *   OB_ADDR + 0  : magic   (uint8_t) — маркер валидности
 *   OB_ADDR + 2  : tc      (int8_t)  — целочисленный счётчик
 *   OB_ADDR + 4  : vc      (int8_t)  — целочисленный счётчик
 *   OB_ADDR + 6  : ml      (uint8_t) — байтовое значение
 *   OB_ADDR + 8  : slots[0] (uint8_t)
 *   OB_ADDR + 10 : slots[1] (uint8_t)
 *   ...
 *   OB_ADDR + 26 : slots[9] (uint8_t)
 */
static struct {
    uint8_t magic;
    int8_t tc;
    int8_t vc;
    uint8_t ml;
    uint8_t slots[SLOTS_COUNT];
} _cache;

/**
 * @brief Считывание данных из flash-области Option Bytes в кэш
 *
 * Читает каждое поле по его адресу в памяти Option Bytes.
 * Используется volatile для предотвращения оптимизации
 * компилятором (чтение напрямую из аппаратного регистра).
 */
static void _refresh(void) {
    _cache.magic = *(volatile uint8_t*)(OB_ADDR);
    _cache.tc    = *(volatile int8_t*)(OB_ADDR + 2);
    _cache.vc    = *(volatile int8_t*)(OB_ADDR + 4);
    _cache.ml    = *(volatile uint8_t*)(OB_ADDR + 6);
    for (uint8_t i = 0; i < SLOTS_COUNT; i++)
        _cache.slots[i] = *(volatile uint8_t*)(OB_ADDR + 8 + i * 2);
}

/**
 * @brief Ожидание завершения операции flash-контроллера
 *
 * Авардит пока бит BSY (Busy) в регистре FLASH->STATR
 * не сбросится, либо пока не истечёт timeout.
 *
 * @param timeout Максимальное количество итераций ожидания
 * @return FLASH_OK — операция завершена, FLASH_TIMEOUT — таймаут
 */
static flash_status_t _wait(uint32_t timeout) {
    while (FLASH->STATR & FLASH_STATR_BSY) {
        if (!--timeout)
            return FLASH_TIMEOUT;
    }
    return FLASH_OK;
}

/**
 * @brief Запись массива байтов в область Option Bytes
 *
 * Каждый байт записывается как 16-битное слово (расширение до uint16_t)
 * со смещением адреса на 2 байта. Это требование hardware CH32V003:
 * option bytes.programмируются по 16-битным словам.
 *
 * @param addr  Начальный адрес записи
 * @param data  Указатель на данные для записи
 * @param size  Количество байтов данных
 * @return FLASH_OK — все байты записаны, FLASH_TIMEOUT — ошибка
 */
static flash_status_t _write_option_bytes(uint32_t addr, const uint8_t *data, uint8_t size) {
    flash_status_t status = FLASH_OK;

    while (size--) {
        /* Запись байта как 16-битного слова (биты [15:8] = 0) */
        *(volatile uint16_t*)addr = *data++;
        status = _wait(0x2000);
        if (status != FLASH_OK)
            break;
        addr += 2;  /* Следующий option byte через 2 байта */
    }
    return status;
}

/**
 * @brief Полный цикл программирования Option Bytes
 *
 * Последовательность операций:
 * 1. Разблокировка flash-контроллера (два ключа)
 * 2. Сохранение текущих стандартных регистров (RDPR..WRPR1)
 * 3. Разблокировка записи option bytes (OBKEYR)
 * 4. Стирание области option bytes (OPTER + STRT)
 * 5. Запись стандартных 6 регистров обратно
 * 6. Запись пользовательских данных (magic, tc, vc, ml, slots[])
 * 7. Блокировка flash-контроллера
 *
 * Стандартные регистры перезаписываются «как есть» для сохранения
 * текущих настроек (защита от стирания при программировании).
 *
 * @return FLASH_OK — всё записано, FLASH_TIMEOUT — ошибка
 */
static flash_status_t _program(void) {
    flash_status_t status;

    /* Шаг 1: Разблокировка flash-контроллера */
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    status = _wait(0x000B0000);
    if (status == FLASH_OK) {
        /* Шаг 2: Сохранение текущих стандартных option bytes */
        uint8_t ob[6];

        ob[0] = OB->RDPR;    /* Защита от чтения */
        ob[1] = OB->USER;    /* Пользовательские опции (IWDG, RESET и т.д.) */
        ob[2] = OB->Data0;   /* Пользовательские данные 0 */
        ob[3] = OB->Data1;   /* Пользовательские данные 1 */
        ob[4] = OB->WRPR0;   /* Защита от записи, страницы 0-7 */
        ob[5] = OB->WRPR1;   /* Защита от записи, страницы 8-15 */

        /* Шаг 3: Разблокировка записи option bytes */
        FLASH->OBKEYR = FLASH_KEY1;
        FLASH->OBKEYR = FLASH_KEY2;

        /* Шаг 4: Стирание области option bytes */
        FLASH->CTLR |= FLASH_CTLR_OPTER;
        FLASH->CTLR |= FLASH_CTLR_STRT;
        status = _wait(0xB0000);
        if (status == FLASH_OK) {
            /* Шаг 5: Переключение в режим программирования */
            FLASH->CTLR &= ~FLASH_CTLR_OPTER;
            FLASH->CTLR |= FLASH_CTLR_OPTPG;

            /* Шаг 6: Запись стандартных 6 регистров */
            status = _write_option_bytes(OB_BASE, ob, sizeof(ob));
            if (status == FLASH_OK) {
                /* Шаг 7: Запись пользовательских данных
                 * Порядок: magic, tc, vc, ml, slots[0..9]
                 * Итого 4 + SLOTS_COUNT = 14 байт
                 */
                uint8_t payload[4 + SLOTS_COUNT];
                payload[0] = _cache.magic;
                payload[1] = (uint8_t)_cache.tc;
                payload[2] = (uint8_t)_cache.vc;
                payload[3] = _cache.ml;
                for (uint8_t i = 0; i < SLOTS_COUNT; i++)
                    payload[4 + i] = _cache.slots[i];
                status = _write_option_bytes(OB_ADDR, payload, sizeof(payload));
            }
        }
        /* Очистка флага программирования (если не было таймаута) */
        if (status != FLASH_TIMEOUT) {
            FLASH->CTLR &= ~FLASH_CTLR_OPTPG;
        }
    }
    /* Блокировка flash-контроллера */
    FLASH->CTLR |= FLASH_CTLR_LOCK;
    return status;
}

/**
 * @brief Инициализация модуля Option Bytes.
 *
 * Считывает данные из flash-области Option Bytes в кэш.
 * Если magic-маркер не совпадает (первый запуск или повреждение),
 * все поля кэша обнуляются.
 * @return true — данные валидны (magic == 0xA5), false — данные невалидны.
 */
bool ob_Init(void) {
    /* Считывание данных из flash в кэш */
    _refresh();

    /* Проверка маркера валидности */
    if (_cache.magic != MAGIC) {
        /* Первый запуск или повреждение — применяем значения по умолчанию */
        _cache.tc = 0;
        _cache.vc = 0;
        _cache.ml = 3;
        _cache.slots[0] = SLOT_DEFAULT_1CH;
        _cache.slots[1] = SLOT_DEFAULT_2CH;
        _cache.slots[2] = SLOT_DEFAULT_3C;
        _cache.slots[3] = SLOT_DEFAULT_3CH;
        _cache.slots[4] = SLOT_DEFAULT_4C;
        _cache.slots[5] = SLOT_DEFAULT_4CH;
        _cache.slots[6] = SLOT_DEFAULT_5C;
        _cache.slots[7] = SLOT_DEFAULT_5CH;
        _cache.slots[8] = SLOT_DEFAULT_6C;
        _cache.slots[9] = SLOT_DEFAULT_6CH;
        return false;
    }
    return true;
}

/**
 * @brief Проверка валидности данных в кэше.
 * @return true — magic-маркер совпадает (0xA5), false — данные невалидны.
 */
bool ob_IsValid(void) {
    return _cache.magic == MAGIC;
}

/**
 * @brief Получение значения счётчика tc.
 * @return Текущее значение tc (int8_t).
 */
int8_t ob_GetTc(void) {
    return _cache.tc;
}

/**
 * @brief Получение значения счётчика vc.
 * @return Текущее значение vc (int8_t).
 */
int8_t ob_GetVc(void) {
    return _cache.vc;
}

/**
 * @brief Получение значения ml.
 * @return Текущее значение ml (uint8_t).
 */
uint8_t ob_GetMl(void) {
    return _cache.ml;
}

/**
 * @brief Установка значения счётчика tc.
 * @param value Новое значение tc.
 */
void ob_SetTc(int8_t value) {
    if(value != _cache.tc)
        change = true;
    _cache.tc = value;
}

/**
 * @brief Установка значения счётчика vc.
 * @param value Новое значение vc.
 */
void ob_SetVc(int8_t value) {
    if(value != _cache.vc)
        change = true;
    _cache.vc = value;
}

/**
 * @brief Установка значения ml.
 * @param value Новое значение ml.
 */
void ob_SetMl(uint8_t value) {
    if(value != _cache.ml)
        change = true;
    _cache.ml = value;
}

/**
 * @brief Чтение элемента массива slots[] по индексу.
 * @param index Индекс элемента (0..SLOTS_COUNT-1).
 * @return Значение элемента или 0 при выходе за границы.
 */
uint8_t ob_GetSlot(uint8_t index) {
    if (index >= SLOTS_COUNT) return 0;
    return _cache.slots[index];
}

/**
 * @brief Запись элемента массива slots[] по индексу.
 * @param index Индекс элемента (0..SLOTS_COUNT-1).
 * @param value Новое значение элемента.
 */
void ob_SetSlot(uint8_t index, uint8_t value) {
    if (index >= SLOTS_COUNT) return;
    if(value != _cache.slots[index])
        change = true;
    _cache.slots[index] = value;
}

/**
 * @brief Сохранение кэша в flash и верификация.
 *
 * Перед записью устанавливает magic-маркер (0xA5), помечая
 * данные как валидные. Выполняет полный цикл записи:
 * разблокировка flash, стирание, запись стандартных регистров
 * и пользовательских данных, затем чтение обратно и сравнение.
 * @return true — все данные записаны и верифицированы, false — ошибка.
 */
bool ob_Flush(void) {

    if (!change)
    {
        return false;
    }
    
    /* Установка маркера валидности перед записью */
    _cache.magic = MAGIC;

    /* Запись кэша в flash */
    if (_program() != FLASH_OK)
        return false;

    /* Верификация: чтение обратно из flash и сравнение с кэшем */
    uint8_t verify[4 + SLOTS_COUNT];
    verify[0] = *(volatile uint8_t*)(OB_ADDR);
    verify[1] = *(volatile uint8_t*)(OB_ADDR + 2);
    verify[2] = *(volatile uint8_t*)(OB_ADDR + 4);
    verify[3] = *(volatile uint8_t*)(OB_ADDR + 6);
    for (uint8_t i = 0; i < SLOTS_COUNT; i++)
        verify[4 + i] = *(volatile uint8_t*)(OB_ADDR + 8 + i * 2);

    /* Сравнение основных полей */
    if (verify[0] != _cache.magic ||
        verify[1] != (uint8_t)_cache.tc ||
        verify[2] != (uint8_t)_cache.vc ||
        verify[3] != _cache.ml)
        return false;

    /* Сравнение массива slots */
    for (uint8_t i = 0; i < SLOTS_COUNT; i++)
        if (verify[4 + i] != _cache.slots[i])
            return false;

    return true;
}
