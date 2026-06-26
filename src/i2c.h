#ifndef I2C_H
#define I2C_H

/*
 * i2c.h — Драйвер I2C1 (прямая запись в регистры)
 *
 * Используется для связи с DAC7571 (управление яркостью LED).
 * Пин-конфигурация: PC1 — SDA, PC2 — SCL (AF_OD, Open-Drain).
 *
 * Функции:
 *   i2c_init()        — полная инициализация I2C1 + GPIO (тактирование, CKCFGR)
 *   i2c_deinit()      — полное отключение I2C1 + GPIO в Hi-Z (экономия тока)
 *   i2c_start()       — генерация START + ожидание SB (Start Bit)
 *   i2c_stop()        — генерация STOP
 *   i2c_send_addr()   — отправка 7-битного адреса + направление (TX/RX)
 *   i2c_send_byte()   — отправка одного байта данных (без проверки ACK)
 *   i2c_wait_ack()    — ожидание подтверждения (BTF) после отправленного байта
 *   i2c_write_byte()  — отправка байта + ожидание ACK одним вызовом
 *
 * Примечание: частота APB1 = 48 МГц (SYSCLK). Все функции блокирующие
 * с таймаутом для предотвращения зависаний.
 *
 * Возвращаемые статусы I2C_OK (0) / I2C_NACK (1) используются в
 * i2c_send_addr(), i2c_wait_ack() и i2c_write_byte() — вызывающий код
 * должен проверять их и завершать транзакцию через i2c_stop() при ошибке
 * (i2c_send_addr() и i2c_wait_ack() сами вызывают STOP при NACK, так что
 * повторный i2c_stop() в этом случае не обязателен, но безопасен).
 */

#include "ch32v00x.h"
#include <stdint.h>

/* Скорости I2C */
#define I2C_SPEED_400K          400000  /* Fast Mode */
#define I2C_SPEED_100K          100000  /* Standard Mode */

/* Направление передачи */
#define I2C_DIR_TX  0   /* Master Transmitter */
#define I2C_DIR_RX  1   /* Master Receiver */

/* Коды возврата */
#define I2C_OK    0   /* Подтверждено (ACK) */
#define I2C_NACK  1   /* Не подтверждено (NACK) или таймаут */

void i2c_init(uint32_t bound);                            /* Инициализация I2C1 */
void i2c_deinit(void);                                    /* Отключение I2C1 (экономия тока) */
void i2c_wait_bus_free(void);                             /* Ожидание освобождения шины I2C (BUSY=0) */
void i2c_start(void);                                     /* START + ожидание SB */
void i2c_stop(void);                                       /* STOP условие */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction);   /* Адрес + направление; I2C_OK/I2C_NACK */
void i2c_send_byte(uint8_t data);                          /* Отправка байта (ждёт только TXE) */
uint8_t i2c_wait_ack(void);                                /* Ожидание BTF/AF после байта; I2C_OK/I2C_NACK */
uint8_t i2c_write_byte(uint8_t data);                       /* send_byte + wait_ack за один вызов */

#endif /* I2C_H */
