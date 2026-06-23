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
    I2C1->CKCFGR = ccr;

    /* 7. Own Address: 7-bit mode, addr = 0 */
    I2C1->OADDR1 = 0x4000;

    /* 8. Включаем I2C + ACK */
    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;
}

/**
 * @brief Генерация START условия на шине I2C
 *
 * Алгоритм:
 *   1. Проверка BUSY флага (STAR2.BUSY) — ждём освобождения шины
 *   2. Установка бита START (CTLR1.START)
 *   3. Ожидание SB флага (STAR1.SB) — START успешно сгенерирован
 *
 * Оба этапа имеют таймаут 100000 итераций.
 */
void i2c_start(void) {
    /* Ждём освобождения шины */
    uint32_t timeout = 100000;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        if (--timeout == 0) return;
    }
    /* Генерируем START */
    I2C1->CTLR1 |= I2C_CTLR1_START;
    /* Ждём завершения */
    timeout = 100000;
    while (!(I2C1->STAR1 & I2C_STAR1_SB)) {
        if (--timeout == 0) return;
    }
}

/**
 * @brief Генерация STOP условия на шине I2C
 */
void i2c_stop(void) {
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
    /* Ждём освобождения шины после STOP */
    uint32_t timeout = 100000;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        if (--timeout == 0) return;
    }
}

/**
 * @brief Отправка 7-битного адреса ведомого
 * @param addr      — 7-битный адрес (например, 0x4C для DAC7571)
 * @param direction — I2C_DIR_TX (запись) или I2C_DIR_RX (чтение)
 *
 * Формат байта на шине: [addr6..addr0][R/W]
 * Адрес сдвигается влево на 1 бит + бит направления.
 *
 * Если ведомый ответил NACK (STAR1.AF), генерируется STOP.
 * После успешного ADDR флага читаются STAR1 и STAR2 для очистки.
 */
void i2c_send_addr(uint8_t addr, uint8_t direction) {
    I2C1->DATAR = (addr << 1) | (direction & 0x01);
    uint32_t timeout = 100000;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR)) {
        /* Проверка NACK (Acknowledge Failure) */
        if (I2C1->STAR1 & I2C_STAR1_AF) {
            I2C1->STAR1 &= ~I2C_STAR1_AF;
            I2C1->CTLR1 |= I2C_CTLR1_STOP;
            return;
        }
        if (--timeout == 0) return;
    }
    /* Очистка флагов ADDR чтением STAR1 и STAR2 */
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
}

/**
 * @brief Отправка одного байта данных
 * @param data — байт для отправки
 *
 * Ожидание флага TXE (Data Register Empty) — регистр данных свободен.
 * Не имеет таймаута (блокируется до отправки).
 */
void i2c_send_byte(uint8_t data) {
    I2C1->DATAR = data;
    uint32_t timeout = 100000;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE)) {
        if (--timeout == 0) return;
    }
}

/**
 * @brief Ожидание ACK от ведомого после отправки данных
 * @return 0 — ACK получен, 1 — NACK или таймаут
 *
 * Проверяет два флага:
 *   TXE — данные отправлены (нет ответа ещё)
 *   AF  — ведомый ответил NACK
 */
uint8_t i2c_wait_ack(void) {
    uint32_t timeout = 10000;
    while (!(I2C1->STAR1 & (I2C_STAR1_TXE | I2C_STAR1_AF))) {
        if (--timeout == 0) return 1;
    }
    if (I2C1->STAR1 & I2C_STAR1_AF) {
        I2C1->STAR1 &= ~I2C_STAR1_AF;
        return 1;
    }
    return 0;
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
