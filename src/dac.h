#ifndef DAC_H
#define DAC_H

/*
 * dac.h — Драйвер ЦАП DAC7571 (I2C, 12-бит)
 *
 * DAC7571 — одноканальный 12-бит ЦАП с I2C интерфейсом.
 * Используется для управления яркостью светодиодов фонарика.
 *
 * Протокол I2C:
 *   START → [адрес][0] → [MSB] → [LSB] → STOP
 *   Адрес: 0x4C (7-бит) / 0x98 (8-бит с битом R/W=0)
 *
 * Формат данных (16 бит после адреса):
 *   [15:14] — режим (00=normal, 01=PD_1K, 10=PD_100K, 11=PD_HiZ)
 *   [13:12] — резерв (00)
 *   [11:0]  — 12-бит значение DAC (0x000..0xFFF)
 *
 * Управление питанием:
 *   PD0 на GPIOD — включение/отключение питания DAC
 *
 * Функции:
 *   dac_pwrpin_init() — настройка GPIO для пина питания
 *   dac_pwrpin_on()   — включить питание DAC
 *   dac_pwrpin_off()  — выключить питание DAC
 *   dac_set_value()   — установить 12-бит значение (нормальный режим)
 *   dac_set_100K()    — перейти в Power-Down 100кОм (экономия тока)
 *
 * Возвращаемые статусы:
 *   dac_set_value() и dac_set_100K() возвращают I2C_OK (0) если DAC
 *   подтвердил приём адреса и обоих байт данных, либо I2C_NACK (1) если
 *   на любом шаге транзакции не пришёл ACK (например, DAC не запитан,
 *   не подключён, или возникла ошибка на шине). Коды I2C_OK/I2C_NACK
 *   определены в i2c.h.
 */

#include "i2c.h"

// Адрес DAC7571 зависит от пина A0:
// Если A0 подключен к GND, 7-битный адрес = 0x4C (бинарно: 1001100)
// Если A0 подключен к VDD, 7-битный адрес = 0x4D (бинарно: 1001101)
#define DAC7571_I2C_ADDR    0x4C

/* Режимы работы DAC7571 (биты 15:14 в формате данных) */
#define DAC7571_MODE_NORMAL    0x00  /* Нормальный режим (активный вывод) */
#define DAC7571_MODE_PD_1K     0x01  /* Power-Down: 1 кОм на GND */
#define DAC7571_MODE_PD_100K   0x02  /* Power-Down: 100 кОм на GND */
#define DAC7571_MODE_PD_HIZ    0x03  /* Power-Down: High Impedance (Hi-Z) */

/* Управление питанием DAC (PD0 на GPIOD) */
#define DAC_PWR_PORT    GPIOD
#define DAC_PWR_PIN     GPIO_Pin_0

void dac_pwrpin_init(void);
void dac_pwrpin_on(void);
void dac_pwrpin_off(void);
uint8_t dac_pwrpin_is_on(void);

uint8_t dac7571_set_voltage(uint16_t data_12bit); /* Установить 12-бит значение; I2C_OK/I2C_NACK */
uint8_t dac7571_power_down(uint8_t mode);

#endif /* DAC_H */
