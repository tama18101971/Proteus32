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
 *
 * @warning Аппаратная особенность CH32V003: стирание Option Bytes (OPTER)
 * стирает страницу целиком (включая RDPR/USER/Data0/Data1/WRPR0/WRPR1).
 * Пропадание питания между стиранием и программированием сбрасывает
 * стандартные регистры в 0xFF (USER=0xFF меняет конфигурацию IWDG/NRST).
 * Для минимизации риска в OB сохраняются исключительно редко меняемые
 * калибровочные параметры (tc, vc, ml), а динамические настройки
 * перенесены в WL_FLASH.
 */

#include <stdint.h>
#include <stdbool.h>
#include "ob.h"
#include "ch32v00x_flash.h"

/* Таймауты операций flash-контроллера */
#define OB_TIMEOUT_WRITE    0x2000u
#define OB_TIMEOUT_UNLOCK   0x000B0000u
#define OB_TIMEOUT_ERASE    0x000B0000u

static bool ob_dirty = false;

/* Лимит подряд неудачных попыток программирования OB: после него flush
 * отказывается работать до следующего изменения кэша. Иначе стойкий сбой
 * (write-защита, деградация питания) приводил бы к полному стиранию
 * OB-страницы каждые 5 с без ограничений — выжигание ресурса NVM. */
#define OB_FLUSH_MAX_FAILS 3u
static uint8_t flush_fails = 0;

/** @brief Пометить кэш грязным и снять лимит неудач предыдущей записи. */
static void ob_mark_dirty(void) {
    ob_dirty = true;
    flush_fails = 0;
}

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
 */
static struct {
    uint8_t magic;
    int8_t tc;
    int8_t vc;
    uint8_t ml;
} _cache;

/**
 * @brief Считывание данных из flash-области Option Bytes в кэш
 *
 * Читает каждое поле по его адресу в памяти Option Bytes.
 * Используется volatile для предотвращения оптимизации
 * компилятором (чтение напрямую из аппаратного регистра).
 */
static void _refresh(void) {
    uint8_t *p = (uint8_t *)&_cache;
    for (uint8_t i = 0; i < sizeof(_cache); i++)
        p[i] = *(volatile uint8_t*)(OB_ADDR + i * 2);
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
        status = _wait(OB_TIMEOUT_WRITE);
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
    status = _wait(OB_TIMEOUT_UNLOCK);
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
        status = _wait(OB_TIMEOUT_ERASE);
        if (status == FLASH_OK) {
            /* Шаг 5: Переключение в режим программирования */
            FLASH->CTLR &= ~FLASH_CTLR_OPTER;
            FLASH->CTLR |= FLASH_CTLR_OPTPG;

            /* Шаг 6: Запись стандартных 6 регистров */
            status = _write_option_bytes(OB_BASE, ob, sizeof(ob));
            if (status == FLASH_OK) {
                /* Шаг 7: Запись пользовательских данных */
                status = _write_option_bytes(OB_ADDR, (const uint8_t *)&_cache, sizeof(_cache));
            }
        }
        /* Сброс режимов стирания и программирования при любом исходе */
        FLASH->CTLR &= ~(FLASH_CTLR_OPTER | FLASH_CTLR_OPTPG);
    }
    /* Блокировка flash-контроллера */
    FLASH->CTLR |= FLASH_CTLR_LOCK;
    return status;
}

/**
 * @brief Заполнение кэша заводскими значениями по умолчанию.
 *
 * Единственный источник дефолтов Option Bytes: используется при первом
 * запуске/повреждении (ob_Init) и при сбросе настроек (ob_ResetToDefaults).
 */
static void _set_defaults(void) {
    _cache.tc = 0;
    _cache.vc = 0;
    _cache.ml = 4;
}

/**
 * @brief Инициализация модуля Option Bytes.
 *
 * Считывает данные из flash-области Option Bytes в кэш.
 * Если magic-маркер не совпадает (первый запуск или повреждение),
 * применяются значения по умолчанию.
 * @return true — данные валидны (magic == 0xA5), false — данные невалидны.
 */
bool ob_Init(void) {
    /* Считывание данных из flash в кэш */
    _refresh();

    /* Проверка маркера валидности */
    if (_cache.magic != MAGIC) {
        /* Первый запуск или повреждение — применяем значения по умолчанию */
        _set_defaults();
        return false;
    }
    return true;
}

/**
 * @brief Сброс кэша Option Bytes к заводским значениям.
 *
 * Помечает кэш грязным; фактическая запись в flash произойдёт
 * при ближайшем вызове ob_Flush() (5-секундный тик персистентности).
 */
void ob_ResetToDefaults(void) {
    _set_defaults();
    ob_mark_dirty();
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
        ob_mark_dirty();
    _cache.tc = value;
}

/**
 * @brief Установка значения счётчика vc.
 * @param value Новое значение vc.
 */
void ob_SetVc(int8_t value) {
    if(value != _cache.vc)
        ob_mark_dirty();
    _cache.vc = value;
}

/**
 * @brief Установка значения ml.
 * @param value Новое значение ml.
 */
void ob_SetMl(uint8_t value) {
    if(value != _cache.ml)
        ob_mark_dirty();
    _cache.ml = value;
}

/**
 * @brief Чтение устаревшего слота из физической памяти Option Bytes.
 *
 * Используется исключительно при однократной миграции v2 -> v3 на первом старте.
 *
 * @param index Индекс слота (0..9).
 * @return Значение слота из OB flash.
 */
uint8_t ob_ReadLegacySlot(uint8_t index) {
    if (index >= 10u) return 0;
    return *(volatile uint8_t *)(OB_ADDR + (4u + index) * 2u);
}

/**
 * @brief Сохранение кэша в flash и верификация.
 *
 * Перед записью устанавливает magic-маркер (0xA5), помечая
 * данные как валидные. Выполняет полный цикл записи:
 * разблокировка flash, стирание, запись стандартных регистров
 * и пользовательских данных, затем чтение обратно и сравнение.
 * @return OB_FLUSH_OK — все данные записаны и верифицированы,
 *         OB_FLUSH_IDLE — кэш не был помечен грязным,
 *         OB_FLUSH_FAIL — ошибка записи или верификации.
 */
ob_flush_status_t ob_Flush(void) {

    if (!ob_dirty)
        return OB_FLUSH_IDLE;

    /* Стойкий сбой: исчерпан лимит попыток. Ожидаем следующего изменения
     * кэша (ob_mark_dirty() обнулит счётчик), чтобы не стирать страницу
     * вхолостую каждые 5 секунд. */
    if (flush_fails >= OB_FLUSH_MAX_FAILS)
        return OB_FLUSH_FAIL;

    /* Установка маркера валидности перед записью */
    _cache.magic = MAGIC;

    /* Запись кэша в flash */
    if (_program() != FLASH_OK)
    {
        flush_fails++;
        return OB_FLUSH_FAIL;
    }

    /* Верификация: чтение обратно из flash и сравнение с кэшем */
    const uint8_t *p = (const uint8_t *)&_cache;
    for (uint8_t i = 0; i < sizeof(_cache); i++)
    {
        if (*(volatile uint8_t *)(OB_ADDR + i * 2) != p[i])
        {
            flush_fails++;
            return OB_FLUSH_FAIL;
        }
    }

    /* Успех: кэш и flash синхронны — снимаем dirty-флаг и счётчик неудач.
     * При провале (выходы выше) dirty сохраняется, но ретрай ограничен
     * лимитом OB_FLUSH_MAX_FAILS. */
    ob_dirty = false;
    flush_fails = 0;
    return OB_FLUSH_OK;
}
