/**
 * @file i2c.h
 * @brief Компактный master-transmitter драйвер I2C1 для CH32V003
 *
 * Замена универсальной библиотеки I2C-CH32V003 ради экономии Flash.
 * Поддерживает ТОЛЬКО тот сценарий, который нужен DAC7571:
 *   START -> ADDR(W) -> байты данных -> STOP
 * Чтение, Repeated START, буферы и сканер отсутствуют. Bus recovery
 * (отпускание SDA, удерживаемой зависшим ведомым: до 9 тактов SCL + STOP)
 * выполняется в каждом i2c_init().
 *
 * Пин-конфигурация: PC1 — SDA, PC2 — SCL (AF Open-Drain).
 * Тактирование: FREQ/CCR выводятся из SYSCLK_HZ (proteus_config.h) под
 * целевую SCL = I2C_SCL_HZ (400 кГц, Fast mode) — без делений в рантайме.
 * Инварианты (точность SCL, границы полей, тайминги Fast I2C по даташиту)
 * закрыты _Static_assert в i2c.c.
 *
 * Коды возврата: I2C_OK (0) или I2C_NACK (1). Любая ошибка — NACK,
 * адрес не подтверждён (DAC обесточен/отсутствует), таймаут или
 * аппаратная ошибка шины (BERR/ARLO).
 */

#ifndef I2C_H
#define I2C_H

#include "ch32v00x.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Коды возврата (совместимы с прежней библиотекой) */
#define I2C_OK      0   /* Успешно / Подтверждено (ACK) */
#define I2C_NACK    1   /* Не подтверждено / ошибка / таймаут */

/* Направление передачи */
#define I2C_DIR_TX  0   /* Master Transmitter */
#define I2C_DIR_RX  1   /* Master Receiver (не используется, оставлено для совместимости) */

/* Основное API */
uint8_t i2c_init(uint32_t bound);       /* Инициализация I2C1 (скорость выводится из SYSCLK_HZ) */
void i2c_deinit(void);                  /* Отключение I2C1, линии PC1/PC2 в Hi-Z */
uint8_t i2c_start(void);                /* START с ожиданием SB */
uint8_t i2c_stop(void);                 /* STOP с ожиданием освобождения шины */
uint8_t i2c_send_addr(uint8_t addr, uint8_t direction); /* Адрес + R/W, ожидание ADDR */
uint8_t i2c_send_byte(uint8_t data);    /* Байт данных, ожидание TXE */
uint8_t i2c_wait_flag(uint16_t flag);   /* Ожидание флага STAR1 (внутреннее API) */

/**
 * @brief Ожидание завершения передачи байта (BTF)
 * @return I2C_OK или I2C_NACK
 *
 * static inline: единственный вызов — из i2c_write_byte(), отдельная
 * extern-функция i2c_wait_ack() в i2c.c только тратила Flash (audit M4, trim).
 */
static inline uint8_t i2c_wait_ack(void)
{
    return i2c_wait_flag(I2C_STAR1_BTF);
}

/**
 * @brief Отправка байта с ожиданием подтверждения (атомарно)
 */
static inline uint8_t i2c_write_byte(uint8_t data)
{
    uint8_t res = i2c_send_byte(data);
    if (res != I2C_OK)
        return res;
    return i2c_wait_ack();
}

#ifdef __cplusplus
}
#endif

#endif /* I2C_H */
