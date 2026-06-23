#include "i2c.h"
#include "ch32v00x.h"

/* RCC bits */
#define RCC_APB2PCENR_IOPCEN  (1 << 4)
#define RCC_APB1PCENR_I2C1EN  (1 << 21)

/* GPIO: AF_OD & 30MHz → CFGLR = (Mode & 0x0F) | Speed = 0x0C | 0x03 = 0x0F */
#define GPIO_CFG_AF_OD_30M  0x0F

/* I2C CKCFGR masks */
#define CKCFGR_FS_Set    ((uint16_t)0x8000)
#define CKCFGR_CCR_Set   ((uint16_t)0x0FFF)

void i2c_init(uint32_t bound) {
    /* 1. Включаем тактирование GPIOC и I2C1 */
    RCC->APB2PCENR |= RCC_APB2PCENR_IOPCEN;
    RCC->APB1PCENR |= RCC_APB1PCENR_I2C1EN;

    /* 2. Конфигурация GPIO: PC1 (SDA) и PC2 (SCL) — AF_OD, 30MHz */
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
    GPIOC->CFGLR |=  (GPIO_CFG_AF_OD_30M << 4)
                   |  (GPIO_CFG_AF_OD_30M << 8);

    /* 3. Software Reset I2C */
    I2C1->CTLR1 |= (1 << 15);  /* SWRST */
    I2C1->CTLR1 &= ~(1 << 15);

    /* 4. Частота тактирования APB1 (в МГц) */
    I2C1->CTLR2 = 48;

    /* 5. Отключаем I2C перед настройкой CKCFGR */
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;

    /* 6. Настройка CKCFGR */
    uint16_t ccr;
    if (bound <= 100000) {
        ccr = 48000000 / (bound << 1);
        if (ccr < 4) ccr = 4;
    } else {
        ccr = 48000000 / (bound * 3);
        if ((ccr & CKCFGR_CCR_Set) == 0) ccr |= 1;
        ccr |= CKCFGR_FS_Set;
    }
    I2C1->CKCFGR = ccr;

    /* 7. Own Address */
    I2C1->OADDR1 = 0x4000;

    /* 8. Включаем I2C + ACK */
    I2C1->CTLR1 = I2C_CTLR1_PE | I2C_CTLR1_ACK;
}

void i2c_start(void) {
    /* Ждём освобождения шины */
    uint32_t timeout = 100000;
    while (I2C1->STAR2 & I2C_STAR2_BUSY) {
        if (--timeout == 0) return;
    }
    I2C1->CTLR1 |= I2C_CTLR1_START;
    timeout = 100000;
    while (!(I2C1->STAR1 & I2C_STAR1_SB)) {
        if (--timeout == 0) return;
    }
}

void i2c_stop(void) {
    I2C1->CTLR1 |= I2C_CTLR1_STOP;
}

void i2c_send_addr(uint8_t addr, uint8_t direction) {
    I2C1->DATAR = (addr << 1) | (direction & 0x01);
    uint32_t timeout = 100000;
    while (!(I2C1->STAR1 & I2C_STAR1_ADDR)) {
        if (I2C1->STAR1 & I2C_STAR1_AF) {
            I2C1->STAR1 &= ~I2C_STAR1_AF;
            I2C1->CTLR1 |= I2C_CTLR1_STOP;
            return;
        }
        if (--timeout == 0) return;
    }
    (void)I2C1->STAR1;
    (void)I2C1->STAR2;
}

void i2c_send_byte(uint8_t data) {
    I2C1->DATAR = data;
    while (!(I2C1->STAR1 & I2C_STAR1_TXE));
}

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

void i2c_deinit(void) {
    I2C1->CTLR1 &= ~I2C_CTLR1_PE;  /* Отключаем I2C */
    RCC->APB1PCENR &= ~RCC_APB1PCENR_I2C1EN;  /* Отключаем тактирование I2C1 */

    /* SDA и SCL в Input Floating (Hi-Z) для экономии тока */
    GPIOC->CFGLR &= ~((0xF << 4) | (0xF << 8));
}
