#include "dac.h"
#include <ch32v00x_i2c.h>

void DAC7571_SetValue(uint16_t data) {
    // Ограничиваем данные 12 битами
    data &= 0x0FFF;

    // Формируем два байта для отправки в нормальном режиме работы (0x00)
    uint8_t msg_msb = (uint8_t)(((DAC7571_MODE_NORMAL << 4) & 0x30) | ((data >> 8) & 0x0F)); 
    uint8_t msg_lsb = (uint8_t)(data & 0xFF);

    while(I2C_GetFlagStatus(I2C1, I2C_FLAG_BUSY));

    I2C_GenerateSTART(I2C1, ENABLE);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C1, DAC7571_ADDRESS, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C1, msg_msb);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_SendData(I2C1, msg_lsb);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_GenerateSTOP(I2C1, ENABLE);
}

void DAC7571_Set100K(void) {
    // Для перевода в режим Power-Down 100кОм, биты PD1:PD0 устанавливаются в 10 (0x02)
    // Данные (12 бит) в этом режиме игнорируются, запишем нули.
    uint8_t msg_msb = (uint8_t)((DAC7571_MODE_PD_100K << 4) & 0x30); 
    uint8_t msg_lsb = 0x00;

    while(I2C_GetFlagStatus(I2C1, I2C_FLAG_BUSY));

    I2C_GenerateSTART(I2C1, ENABLE);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_MODE_SELECT));

    I2C_Send7bitAddress(I2C1, DAC7571_ADDRESS, I2C_Direction_Transmitter);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED));

    I2C_SendData(I2C1, msg_msb);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_SendData(I2C1, msg_lsb);
    while(!I2C_CheckEvent(I2C1, I2C_EVENT_MASTER_BYTE_TRANSMITTED));

    I2C_GenerateSTOP(I2C1, ENABLE);
}
