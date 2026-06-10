#include "i2c.h"
#include <ch32v00x_i2c.h>
#include <ch32v00x_rcc.h>
#include <ch32v00x_gpio.h>

void I2C1_Init(uint32_t bound) {
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    I2C_InitTypeDef  I2C_InitStructure = {0};

    // Включаем тактирование GPIOC и I2C1
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    // Конфигурация пинов: PC1 - SDA, PC2 - SCL (Альтернативная функция, Открытый сток)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // Настройка периферийного модуля I2C
    I2C_InitStructure.I2C_ClockSpeed = bound;
    I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_2;
    I2C_InitStructure.I2C_OwnAddress1 = 0x00;
    I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C1, &I2C_InitStructure);

    // Включаем I2C1
    I2C_Cmd(I2C1, ENABLE);
}
