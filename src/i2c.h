#ifndef I2C_H
#define I2C_H

/*
 * i2c.h — Драйвер I2C1 (прямая запись в регистры)
 *
 * Используется для связи с DAC7571 (управление яркостью LED).
 * Пин-конфигурация: PC1 — SDA, PC2 — SCL (AF_OD, Open-Drain).
 *
 * Функции:
 *   i2c_init()      — полная инициализация I2C1 + GPIO (тактирование, CKCFGR)
 *   i2c_deinit()    — полное отключение I2C1 + GPIO в Hi-Z (экономия тока)
 *   i2c_start()     — генерация START + ожидание SB (Start Bit)
 *   i2c_stop()      — генерация STOP
 *   i2c_send_addr() — отправка 7-битного адреса + направление (TX/RX)
 *   i2c_send_byte() — отправка одного байта данных
 *   i2c_wait_ack()  — ожидание ACK/NACK от ведомого
 *
 * Примечание: частота APB1 = 48 МГц (SYSCLK). Все функции блокирующие
 * с таймаутом для предотвращения зависаний.
 */

#include "ch32v00x.h"
#include <stdint.h>

/* Скорости I2C */
#define I2C_SPEED_400K          400000  /* Fast Mode */
#define I2C_SPEED_100K          100000  /* Standard Mode */

/* Направление передачи */
#define I2C_DIR_TX  0   /* Master Transmitter */
#define I2C_DIR_RX  1   /* Master Receiver */

void i2c_init(uint32_t bound);                              /* Инициализация I2C1 */
void i2c_deinit(void);                                      /* Отключение I2C1 (экономия тока) */
void i2c_start(void);                                       /* START + ожидание SB */
void i2c_stop(void);                                        /* STOP条件 */
void i2c_send_addr(uint8_t addr, uint8_t direction);        /* Отправка адреса (7-бит) */
void i2c_send_byte(uint8_t data);                           /* Отправка байта */
uint8_t i2c_wait_ack(void);                                 /* Ожидание ACK (0=OK, 1=NACK) */

#endif /* I2C_H */
