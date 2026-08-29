/**
 * @file i2c.c
 * @brief Компактный master-transmitter драйвер I2C1 для CH32V003
 *
 * Компактная замена библиотеки I2C-CH32V003 (экономия Flash).
 * Только передача (Master Transmitter): START -> ADDR(W) -> data -> STOP.
 *
 * Конфигурация фиксирована: SYSCLK = 48 МГц, SCL = 400 кГц:
 *   CTLR2.FREQ = 48, CKCFGR = FS | CCR=40 (48М/(400к*3)), TRISE = 49,
 *   OADDR1 = бит14 (обязателен по спецификации WCH/Synopsys).
 * Таймауты — простой счётчик циклов (~10 мс @ 48 МГц), без делений.
 *
 * Обработка ошибок: AF (NACK) / BERR / ARLO / таймаут → все сворачиваются
 * в I2C_NACK. При NACK адреса или ошибке шины автоматически выдаётся STOP,
 * чтобы не оставить шину занятой (BUSY) перед следующей попыткой.
 */

#include "i2c.h"

/* Таймаут ожидания флага в итерациях цикла (~4 такта/итерацию ≈ 10 мс @ 48 МГц) */
#define I2C_TIMEOUT_LOOPS   120000u

/* Конфигурация линий PC1 (SDA) / PC2 (SCL): AF Open-Drain, 50 МГц */
#define I2C_PINS_AF_OD      0x00000FF0u
#define I2C_PINS_MASK       0x00000FF0u

/**
 * @brief Ожидание установки бита флага в STAR1 с контролем ошибок и таймаута
 *
 * При AF (NACK ведомого) и BERR/ARLO формируется STOP (шина не остаётся
 * занятой) и возвращается I2C_NACK. Флаги ошибок сбрасываются записью
 * инвертированной маски (требование WCH).
 *
 * @param flag Бит флага STAR1 (SB/ADDR/TXE/BTF)
 * @return I2C_OK или I2C_NACK
 */
static uint8_t i2c_wait_flag(uint16_t flag)
{
    uint32_t t = I2C_TIMEOUT_LOOPS;

    while (!(I2C1->STAR1 & flag))
    {
        uint16_t star1 = I2C1->STAR1;

        if (star1 & I2C_STAR1_AF)
        {
            I2C1->STAR1 = (uint16_t)~I2C_STAR1_AF;
            I2C1->CTLR1 |= I2C_CTLR1_STOP;
            return I2C_NACK;
        }
        if (star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO))
        {
            I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO);
            I2C1->CTLR1 |= I2C_CTLR1_STOP;
            return I2C_NACK;
        }
        if (--t == 0u)
            return I2C_NACK;    /* Таймаут: вызывающий код сам решает, слать ли STOP */
    }
    return I2C_OK;
}

/**
 * @brief Инициализация I2C1 (PC1=SDA, PC2=SCL, 400 кГц @ 48 МГц)
 *
 * Параметр bound сохранён для совместимости сигнатур, фактически
 * скорость фиксирована константами под SYSCLK 48 МГц.
 *
 * @param bound Требуемая скорость (игнорируется; см. описание)
 * @return I2C_OK
 */
uint8_t i2c_init(uint32_t bound)
{
    (void)bound;

    RCC->APB2PCENR |= RCC_IOPCEN;   /* Порт C */
    RCC->APB1PCENR |= RCC_I2C1EN;   /* I2C1 */

    GPIOC->CFGLR = (GPIOC->CFGLR & ~I2C_PINS_MASK) | I2C_PINS_AF_OD;

    I2C1->CTLR1 |= I2C_CTLR1_SWRST;     /* Программный сброс */
    I2C1->CTLR1 &= (uint16_t)~I2C_CTLR1_SWRST;

    I2C1->CTLR2  = 48u;                 /* PCLK1 в МГц */
    I2C1->CKCFGR = I2C_CKCFGR_FS | 40u; /* Fast mode, 48М/(400к*3) = 40 */
    I2C1->OADDR1 = (1u << 14);          /* Обязательный бит 14 (WCH) */

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
 * Ожидает освобождения шины (BUSY=0), выставляет START, ждёт SB.
 *
 * @return I2C_OK или I2C_NACK
 */
uint8_t i2c_start(void)
{
    uint32_t t = I2C_TIMEOUT_LOOPS;

    while (I2C1->STAR2 & I2C_STAR2_BUSY)
    {
        if (--t == 0u)
            return I2C_NACK;
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

/**
 * @brief Ожидание завершения передачи байта (BTF)
 * @return I2C_OK или I2C_NACK
 */
uint8_t i2c_wait_ack(void)
{
    return i2c_wait_flag(I2C_STAR1_BTF);
}
