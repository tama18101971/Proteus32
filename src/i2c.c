/*
 * i2c.c — Драйвер I2C1 (прямая запись в регистры CH32V003)
 *
 * Конфигурация:
 *   - Шина: I2C1 (PC1=SDA, PC2=SCL)
 *   - Режим: Master Transmitter (запись в DAC7571)
 *   - Скорость: Fast Mode (400 кГц) или Standard Mode (100 кГц)
 *   - Тактирование: APB1 = 48 МГц (SYSCLK)
 *
 * Формула CCR:
 *   Standard Mode: CCR = PCLK1 / (2 × FI2C)
 *   Fast Mode (Duty=2): CCR = PCLK1 / (3 × FI2C)
 *
 * Регистры (прямой доступ):
 *   I2C1->CTLR1   — управление (PE, START, STOP, ACK, SWRST)
 *   I2C1->CTLR2   — частота тактирования (FREQ)
 *   I2C1->CKCFGR   — настройка скорости (CCR, F/S)
 *   I2C1->OADDR1   — собственный адрес
 *   I2C1->DATAR    — регистр данных (TX/RX)
 *   I2C1->STAR1    — статус 1 (SB, ADDR, BTF, TXE, RXNE, AF)
 *   I2C1->STAR2    — статус 2 (BUSY, MSL)
 */

#include "i2c.h"
#include "ch32v00x.h"

/* RCC: включение тактирования периферии */
#define RCC_APB2PCENR_IOPCEN  (1 << 4)   /* GPIOC clock enable */
#define RCC_APB1PCENR_I2C1EN  (1 << 21)  /* I2C1 clock enable */

/* GPIO: AF_OD (Alternate Function Open-Drain) + 30MHz
 * CFGLR = (Mode & 0x0F) | Speed = 0x0C | 0x03 = 0x0F
 * Важно: SDK маскирует Mode через & 0x0F перед записью в CFGLR! */
#define GPIO_CFG_AF_OD_30M  0x0F

/* CKCFGR masks */
#define CKCFGR_FS_Set    ((uint16_t)0x8000)  /* Bit 15: F/S (0=Standard, 1=Fast) */
#define CKCFGR_CCR_Set   ((uint16_t)0x0FFF)  /* Bits 11:0: Clock Control Register */

/* OADDR1: бит 14 должен быть установлен в 1 согласно datasheet (зарезервирован
 * под совместимость с 0xCxxx ref.man addressing scheme); биты 7:1 — адрес. */
#define OADDR1_REQUIRED_BIT14   (1 << 14)

/* Таймауты (число итераций цикла опроса, не время в мкс/мс).
 * 100000 итераций на 48 МГц соответствует масштабу в десятки мс —
 * достаточно, чтобы не зависнуть, но не словить ложный таймаут при
 * нормальной работе шины. */
#define I2C_TIMEOUT_BUS_FREE   100000
#define I2C_TIMEOUT_EVENT      100000
#define I2C_TIMEOUT_ACK        100000

/**
 * @brief Инициализация I2C1
 * @param bound — желаемая частота шины (100000 или 400000)
 *
 * Последовательность:
 *   1. Включение тактирования GPIOC + I2C1
 *   2. Настройка GPIO (PC1=SDA, PC2=SCL → AF_OD, 30MHz)
 *   3. Software Reset I2C (SWRST)
 *   4. Установка частоты APB1 в CTLR2
 *   5. Отключение PE перед настройкой CKCFGR
 *   6. Расчёт и запись CCR (Standard/Fast Mode)
 *   7. Настройка Own Address (7-bit, addr=0)
 *   8. Включение I2C + ACK
 */
void i2c_init(uint32_t bound) {
    /* 1. Включаем тактирование GPIOC и I2C1 */
    RCC->APB2PCENR |= RCC_APB2PCENR_IOPCEN;
    RCC->APB1PCENR |= RCC_APB1PCENR_I2C1EN;

    /* 2. Конфигурация GPIO: PC1 (SDA) и PC2 (SCL) — AF_OD, 30MHz */
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_AF_OD_30M << 4)
                   |  (GPIO_CFG_AF_OD_30M << 8);

    /* 3. Software Reset I2C — сброс всех регистров */
    I2C1->CTLR1 |= (1 << 15);  /* SWRST = 1 */
    I2C1->CTLR1 &= ~(1 << 15); /* SWRST = 0 */

    /* 4. Частота тактирования APB1 (в МГц) для расчёта CCR */
    I2C1->CTLR2 = 48;

    /* 5. Отключаем I2C перед настройкой CKCFGR (требование datasheet) */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;

    /* 6. Настройка CKCFGR (Clock Control Register) */
    uint16_t ccr;
    if (bound <= 100000) {
        /* Standard Mode: CCR = PCLK1 / (2 × FI2C) */
        ccr = 48000000 / (bound << 1);
        if (ccr < 4) ccr = 4;
    } else {
        /* Fast Mode (Duty=2): CCR = PCLK1 / (3 × FI2C) */
        ccr = 48000000 / (bound * 3);
        if ((ccr & CKCFGR_CCR_Set) == 0) ccr |= 1;
        ccr |= CKCFGR_FS_Set;
    }
    /* Защита от переполнения 12-битного поля CCR (на случай нестандартных bound) */
    if ((ccr & CKCFGR_CCR_Set) > CKCFGR_CCR_Set) {
        ccr = (ccr & CKCFGR_FS_Set) | CKCFGR_CCR_Set;
    }
    I2C1->CKCFGR = ccr;

    /* 7. Own Address: 7-bit mode, addr = 0. Бит 14 зарезервирован и должен
     * быть установлен в 1 (см. описание OADDR1 в reference manual). */
    I2C1->OADDR1 = OADDR1_REQUIRED_BIT14;

    /* 8. Включаем I2C + ACK */
    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;
}

/**
 * @brief Ожидание освобождения шины I2C (флаг BUSY=0)
 *
 * Проверяет STAR2.BUSY. Если шина занята — ждёт с таймаутом.
 * Используется после STOP или при восстановлении после ошибки.
 */
void i2c_wait_bus_free(void) {
    uint32_t timeout = I2C_TIMEOUT_BUS_FREE;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        if (--timeout == 0) return;
    }
}

/**
 * @brief Генерация START условия на шине I2C
 *
 * Алгоритм:
 *   1. Проверка BUSY флага (STAR2.BUSY) — ждём освобождения шины
 *   2. Установка бита START (CTLR1.START)
 *   3. Ожидание SB флага (STAR1.SB) — START успешно сгенерирован
 *
 * Оба этапа имеют таймаут. Примечание: эта функция предполагает обычный
 * (не repeated) START — перед вызовом шина должна быть свободна.
 */
void i2c_start(void) {
    i2c_wait_bus_free();
    /* Генерируем START */
    I2C1->CTLR1 |= I2C_CTLR1_START;
    /* Ждём завершения */
    uint32_t timeout = I2C_TIMEOUT_EVENT;
    while (!(I2C1->STAR1 & I2C_STAR1_SB)) {
        if (--timeout == 0) return;
    }
}

/**
 * @brief Генерация STOP условия на шине I2C
 */
void i2c_stop(void) {
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    i2c_wait_bus_free();
}

/**
 * @brief Отправка 7-битного адреса ведомого
 * @param addr      — 7-битный адрес (например, 0x4C для DAC7571)
 * @param direction — I2C_DIR_TX (запись) или I2C_DIR_RX (чтение)
 * @return I2C_OK (0) если адрес подтверждён (ADDR=1), I2C_NACK (1) если
 *         ведомый ответил NACK или произошёл таймаут.
 *
 * Формат байта на шине: [addr6..addr0][R/W]
 * Адрес сдвигается влево на 1 бит + бит направления.
 *
 * Если ведомый ответил NACK (STAR1.AF), генерируется STOP.
 * После успешного ADDR флага читаются STAR1 и STAR2 для очистки.
 */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction) {
    I2C1->DATAR = (addr << 1) | (direction & 0x01);
    uint32_t timeout = I2C_TIMEOUT_EVENT;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR)) {
        /* Проверка NACK (Acknowledge Failure) */
        if (I2C1->STAR1 & I2C_STAR1_AF) {
            I2C1->STAR1 &= ~I2C_STAR1_AF;
            I2C1->CTLR1 |= I2C_CTLR1_STOP;
            i2c_wait_bus_free();
            return I2C_NACK;
        }
        if (--timeout == 0) return I2C_NACK;
    }
    /* Очистка флагов ADDR чтением STAR1 и STAR2 */
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
    return I2C_OK;
}

/**
 * @brief Отправка одного байта данных в регистр DATAR
 * @param data — байт для отправки
 *
 * Ожидает флаг TXE (Data Register Empty) — означает только, что байт
 * перенесён в сдвиговый регистр и DATAR свободен для следующего байта.
 * TXE НЕ означает, что ведомый подтвердил приём (см. i2c_wait_ack()).
 * Имеет таймаут — при обрыве шины не зависает навечно.
 */
void i2c_send_byte(uint8_t data) {
    I2C1->DATAR = data;
    uint32_t timeout = I2C_TIMEOUT_EVENT;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE)) {
        if (--timeout == 0) return;
    }
}

/**
 * @brief Ожидание подтверждения приёма байта ведомым
 * @return I2C_OK (0) — ACK получен, I2C_NACK (1) — NACK или таймаут
 *
 * ВАЖНО: проверяется флаг BTF (Byte Transfer Finished), а не TXE.
 * TXE взводится сразу после переноса байта в сдвиговый регистр — то есть
 * ДО того, как бит ACK реально придёт с шины от ведомого. Если ждать TXE
 * здесь, функция почти всегда вернёт "OK" мгновенно, не дождавшись
 * настоящего ответа устройства. BTF взводится только после получения
 * бита ACK/NACK, поэтому именно он даёт достоверный результат.
 */
uint8_t i2c_wait_ack(void) {
    uint32_t timeout = I2C_TIMEOUT_ACK;
    while (!(I2C1->STAR1 & (I2C_STAR1_BTF | I2C_STAR1_AF))) {
        if (--timeout == 0) return I2C_NACK;
    }
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 &= ~I2C_STAR1_AF;
        I2C1->CTLR1 |= I2C_CTLR1_STOP;
        i2c_wait_bus_free();
        return I2C_NACK;
    }
    return I2C_OK;
}

/**
 * @brief Отправка байта с ожиданием подтверждения — удобный wrapper
 * @param data — байт для отправки
 * @return I2C_OK (0) если ведомый подтвердил приём, I2C_NACK (1) иначе
 *
 * Объединяет i2c_send_byte() + i2c_wait_ack() для типового случая
 * "отправить байт и убедиться, что он принят", что покрывает большинство
 * вызовов в коде работы с DAC7571.
 */
uint8_t i2c_write_byte(uint8_t data) {
    i2c_send_byte(data);
    return i2c_wait_ack();
}

/**
 * @brief Полное отключение I2C1 (для экономии энергии перед сном)
 *
 * Выполняет:
 *   1. Отключение I2C (PE=0)
 *   2. Отключение тактирования I2C1 (APB1PCENR)
 *   3. Перевод GPIO в Input Floating (Hi-Z) — убирает утечку через pull-up
 */
void i2c_deinit(void) {
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;
    RCC->APB1PCENR &= ~RCC_APB1PCENR_I2C1EN;

    /* SDA и SCL в Input Floating (Hi-Z) */
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
}
