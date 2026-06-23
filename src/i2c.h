#ifndef I2C_H
#define I2C_H

#include "ch32v00x.h"

#define I2C_SPEED_400K          400000
#define I2C_SPEED_100K          100000

#define I2C_DIR_TX  0
#define I2C_DIR_RX  1

void i2c_init(uint32_t bound);
void i2c_start(void);
void i2c_stop(void);
void i2c_send_addr(uint8_t addr, uint8_t direction);
void i2c_send_byte(uint8_t data);
uint8_t i2c_wait_ack(void);
void i2c_deinit(void);

#endif /* I2C_H */
