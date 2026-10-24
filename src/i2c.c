// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file i2c.c
 * @brief Компактный master-transmitter драйвер I2C1 для CH32V003
 *
 * Компактная замена библиотеки I2C-CH32V003 (экономия Flash).
 * Только передача (Master Transmitter): START -> ADDR(W) -> data -> STOP.
 *
 * Тактирование выводится из SYSCLK_HZ (proteus_config.h); целевая частота
 * SCL — I2C_SCL_HZ (400 кГц, Fast mode, DUTY=0):
 *   CTLR2.FREQ = SYSCLK в МГц, CKCFGR = FS | CCR, где CCR = SYSCLK/(3*SCL),
 *   OADDR1 = бит14 (обязателен по спецификации WCH/Synopsys).
 * Таймауты — простой счётчик циклов (SPIN_LOOPS_MS), без делений.
 *
 * Обработка ошибок: AF (NACK) / BERR / ARLO / таймаут → все сворачиваются
 * в I2C_NACK. При NACK адреса, ошибке шины или таймауте автоматически
 * выдаётся STOP, чтобы оборванная транзакция не держала шину занятой
 * (BUSY) — иначе все следующие i2c_start() падали бы по таймауту до
 * переинициализации (её выполняет dac7571_set_voltage() при первом сбое).
 *
 * Восстановление шины (bus recovery): каждый i2c_init() перед настройкой
 * AF отпускает SDA, удерживаемую зависшим ведомым, — до 9 тактов SCL в
 * GPIO-режиме (audit M4: SWRST-реинит шину в этом случае не освобождал,
 * и яркость «залипала» до перезапуска). STOP при этом НЕ выдаётся: на
 * незанятой шине бит STOP не расходуется и рвёт первую транзакцию
 * (см. i2c_init()).
 */

#include "i2c.h"
#include "proteus_config.h"

/* ── Тактирование I2C1 (всё выводится из SYSCLK_HZ) ────────────────
 * Fast mode при DUTY=0: T(SCL) = 3 * CCR / PCLK1, то есть
 * SCL = PCLK1 / (3 * CCR). PCLK1 == SYSCLK (у CH32V003 нет делителя APB). */
#define I2C_SCL_HZ          400000u
#define I2C_FREQ_MHZ        (SYSCLK_HZ / 1000000u)
#define I2C_CCR             (SYSCLK_HZ / (3u * I2C_SCL_HZ))

/* CCR должен получиться без остатка, иначе фактическая SCL уедет от
 * заявленной, а ассерт round-trip ниже это молча пропустил бы. */
_Static_assert(SYSCLK_HZ % (3u * I2C_SCL_HZ) == 0u,
               "SYSCLK_HZ does not divide evenly into the I2C CCR");
_Static_assert(SYSCLK_HZ / (3u * I2C_CCR) == I2C_SCL_HZ,
               "I2C CCR does not reproduce the requested SCL frequency");
/* Ширина поля CKCFGR.CCR[11:0] (RM 13.11.8). */
_Static_assert(I2C_CCR >= 1u && I2C_CCR <= 0x0FFFu,
               "I2C CCR outside the 12-bit CKCFGR field");
/* Поле CTLR2.FREQ[5:0] допускает 001000b..110000b = 8..48 МГц (RM 13.11.2). */
_Static_assert(I2C_FREQ_MHZ >= 8u && I2C_FREQ_MHZ <= 48u,
               "I2C CTLR2.FREQ outside the documented 8..48 MHz range");
/* Тайминги Fast I2C (DS табл. 3-21): при DUTY=0 низкий полупериод равен
 * 2*CCR тактов PCLK1, высокий — CCR. Считаем в наносекундах. */
_Static_assert((1000u * I2C_CCR) / I2C_FREQ_MHZ >= 600u,
               "I2C SCL high time below the 600 ns Fast-mode minimum");
_Static_assert((2000u * I2C_CCR) / I2C_FREQ_MHZ >= 1200u,
               "I2C SCL low time below the 1200 ns Fast-mode minimum");

/* Таймаут ожидания флага в итерациях цикла (~10 мс).
 * Принято по аудиту (сентябрь 2026): бюджет рассчитан на застрявшую шину,
 * а не на clock stretching — DAC7571 не документирует растяжение
 * (write-only ЦАП, данные защёлкиваются на ACK), полная транзакция
 * ~0.8 мс при 400 кГц. Восстановление трёхслойное: STOP по таймауту /
 * NACK (i2c_wait_flag), STOP при застрявшем BUSY (i2c_start), SWRST-reinit
 * при первом же сбое (dac7571_set_voltage). Повышение до 20 мс лишь
 * замедлило бы обнаружение реального клина, пользы нет. */
#define I2C_TIMEOUT_LOOPS   SPIN_LOOPS_MS(10)

/* Конфигурация линий PC1 (SDA) / PC2 (SCL): AF Open-Drain, 50 МГц */
#define I2C_PINS_AF_OD      0x00000FF0u
#define I2C_PINS_MASK       0x00000FF0u

/**
 * @brief Аварийный выход из транзакции: STOP + I2C_NACK.
 *
 * Общий хвост всех путей ошибки i2c_wait_flag() (noinline — одна копия
 * вместо трёх встроенных, экономия Flash). STOP обязателен: без него
 * периферия остаётся мастером с занятой шиной (BUSY), и все следующие
 * i2c_start() падают по таймауту вплоть до переинициализации.
 */
static __attribute__((noinline)) uint8_t i2c_fail_stop(void)
{
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    return I2C_NACK;
}

/**
 * @brief Ожидание установки бита флага в STAR1 с контролем ошибок и таймаута
 *
 * При AF (NACK ведомого), BERR/ARLO и таймауте формируется STOP (шина не
 * остаётся занятой) и возвращается I2C_NACK. Флаги STAR1 — rc_w0 и на пути
 * ошибки сбрасываются записью нуля во весь регистр (см. тело).
 *
 * @param flag Бит флага STAR1 (SB/ADDR/TXE/BTF)
 * @return I2C_OK или I2C_NACK
 *
 * Не static: вызывается также из inline-обёрток i2c.h (i2c_wait_ack).
 */
uint8_t i2c_wait_flag(uint16_t flag)
{
    uint32_t t = I2C_TIMEOUT_LOOPS;

    while (!(I2C1->STAR1 & flag))
    {
        uint16_t star1 = I2C1->STAR1;

        /* AF (NACK ведомого), BERR и ARLO сворачиваются в один выход.
         * Транзакция уже оборвана, поэтому вместо инвертированной маски
         * (дорогая 32-битная константа) в STAR1 пишется ноль: флаги STAR1
         * — rc_w0, запись 0 сбрасывает их все разом, read-only биты не
         * затрагиваются, а сброс TXE/BTF/SB/ADDR на пути ошибки безвреден
         * (транзакция завершается STOP-ом). */
        if (star1 & (I2C_STAR1_AF | I2C_STAR1_BERR | I2C_STAR1_ARLO))
        {
            I2C1->STAR1 = 0;
            return i2c_fail_stop();
        }
        if (--t == 0u)
        {
            /* Таймаут: транзакция оборвана посреди передачи (см. i2c_fail_stop;
             * переинициализацию выполняет dac7571_set_voltage()). */
            return i2c_fail_stop();
        }
    }
    return I2C_OK;
}

/**
 * @brief Инициализация I2C1 (PC1=SDA, PC2=SCL, I2C_SCL_HZ при SYSCLK_HZ)
 *
 * Параметр bound сохранён для совместимости сигнатур, фактически
 * скорость задаётся константами, выведенными из SYSCLK_HZ.
 *
 * @param bound Требуемая скорость (игнорируется; см. описание)
 * @return I2C_OK
 */
uint8_t i2c_init(uint32_t bound)
{
    (void)bound;

    RCC->APB2PCENR |= RCC_IOPCEN;   /* Порт C */
    RCC->APB1PCENR |= RCC_I2C1EN;   /* I2C1 */

    /* Восстановление шины (audit M4): зависший посреди транзакции ведомый
     * может удерживать SDA внизу — AF-перенастройка и SWRST шину не
     * освобождают, и все последующие транзакции падали бы до перезапуска.
     * PC1=SDA ввод, PC2=SCL выход open-drain: ровно 9 тактов SCL — этого
     * достаточно, чтобы вытолкнуть зависший бит-поток ведомого и отпустить
     * SDA (свободная шина: DAC без START такты игнорирует, цена ~6 мкс).
     * Программных задержек нет: cadence записей в регистры даёт
     * SCL ~1-2 МГц, а единственный ведомый — DAC7571 с HS-входом
     * (до 3.4 Мбит/с) — фронты гарантированно регистрируются.
     * STOP периферией не выдаётся (см. финальную запись CTLR1 ниже:
     * STOP на незанятой шине рвёт первую же мастер-транзакцию). */
    GPIOC->CFGLR = (GPIOC->CFGLR & ~I2C_PINS_MASK) | 0x00000640u;
    uint8_t pulses = 9u;
    do
    {
        GPIOC->BCR  = (1u << 2);                /* SCL = 0 */
        GPIOC->BSHR = (1u << 2);                /* SCL = 1 */
    } while (--pulses);

    GPIOC->CFGLR = (GPIOC->CFGLR & ~I2C_PINS_MASK) | I2C_PINS_AF_OD;

    /* Программный сброс. Абсолютные записи вместо read-modify-write:
     * init-путь всё равно переконфигурирует CTLR1 с нуля, а пара
     * li+sw / sw zero короче двух RMW (экономия Flash, audit M4 trim). */
    I2C1->CTLR1 = I2C_CTLR1_SWRST;
    I2C1->CTLR1 = 0;

    I2C1->CTLR2  = (uint16_t)I2C_FREQ_MHZ;              /* PCLK1 в МГц */
    I2C1->CKCFGR = (uint16_t)(I2C_CKCFGR_FS | I2C_CCR); /* Fast mode + CCR */
    I2C1->OADDR1 = (1u << 14);          /* Обязательный бит 14 (WCH) */

    /* PE + ACK одной записью. STOP здесь не ставится: STOP — действие
     * мастера (генерация Stop-условия). На незанятой шине бит не
     * расходуется, а срабатывает при первом же входе интерфейса в
     * мастер-режим — сразу после адресного байта транзакции. Итог:
     * каждая транзакция рвётся на START->ADDR->STOP, DAC7571 не
     * защёлкивает данные и основной диод не загорается (регрессия
     * 1aa2965). Восстановление ведомого после recovery-тактов STOP-ом
     * не требуется: единственный ведомый — write-only ЦАП, кадр
     * защёлкивается по счёту тактов, а не по STOP. */
    I2C1->CTLR1 |= (I2C_CTLR1_PE | I2C_CTLR1_ACK);
    return I2C_OK;
}

/**
 * @brief Полное отключение I2C1, линии PC1/PC2 → Hi-Z (Analog)
 */
void i2c_deinit(void)
{
    I2C1->CTLR1 &= (uint16_t)~I2C_CTLR1_PE;
    RCC->APB1PCENR &= (uint32_t)~RCC_I2C1EN;
    GPIOC->CFGLR &= ~I2C_PINS_MASK;     /* PC1/PC2 в аналоговый вход (Hi-Z) */
}

/**
 * @brief Генерация START условия
 *
 * Ожидает освобождения шины (BUSY=0), выставляет START, ждёт SB. Залипший
 * BUSY после оборванной транзакции закрывается STOP-ом по таймауту.
 *
 * @return I2C_OK или I2C_NACK
 */
uint8_t i2c_start(void)
{
    uint32_t t = I2C_TIMEOUT_LOOPS;

    while (I2C1->STAR2 & I2C_STAR2_BUSY)
    {
        if (--t == 0u)
        {
            /* BUSY не освободился (шиной владеет оборванная транзакция):
             * пробуем закрыть её STOP-ом; если мастер давно не активен,
             * запись безвредна. Полное восстановление — у вызывающего кода. */
            return i2c_fail_stop();
        }
    }

    I2C1->CTLR1 |= I2C_CTLR1_START;
    return i2c_wait_flag(I2C_STAR1_SB);
}

/**
 * @brief Генерация STOP условия с ожиданием освобождения шины
 * @return I2C_OK или I2C_NACK
 */
uint8_t i2c_stop(void)
{
    uint32_t t = I2C_TIMEOUT_LOOPS;

    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    while (I2C1->STAR2 & I2C_STAR2_BUSY)
    {
        if (--t == 0u)
            return I2C_NACK;
    }
    return I2C_OK;
}

/**
 * @brief Отправка адреса ведомого с битом направления, ожидание ADDR
 *
 * После установки ADDR выполняется чтение STAR1+STAR2 для его сброса.
 *
 * @param addr 7-битный адрес ведомого
 * @param direction I2C_DIR_TX или I2C_DIR_RX
 * @return I2C_OK или I2C_NACK
 */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction)
{
    uint8_t res;

    I2C1->DATAR = (uint16_t)((addr << 1) | (direction & 1u));
    res = i2c_wait_flag(I2C_STAR1_ADDR);
    if (res != I2C_OK)
        return res;

    (void)I2C1->STAR1;  /* Последовательность очистки ADDR: */
    (void)I2C1->STAR2;  /* чтение STAR1, затем STAR2 */
    return I2C_OK;
}

/**
 * @brief Отправка одного байта данных (ожидание TXE, без контроля ACK)
 * @param data Байт данных
 * @return I2C_OK или I2C_NACK
 */
uint8_t i2c_send_byte(uint8_t data)
{
    I2C1->DATAR = data;
    return i2c_wait_flag(I2C_STAR1_TXE);
}

/* i2c_wait_ack() перенесён в i2c.h (static inline): единственный вызов —
 * из i2c_write_byte(), отдельная extern-копия только тратила Flash. */
