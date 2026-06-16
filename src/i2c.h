#ifndef I2C_H
#define I2C_H

#include "ch32v00x.h"

#define I2C_SPEED_400K          400000 // 400 кГц (Fast Mode)
#define I2C_SPEED_100K          100000 // 100 кГц (Standard Mode)

void i2c_init(uint32_t bound);

#endif /* I2C_H */
