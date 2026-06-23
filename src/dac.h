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
 */

#include "ch32v003_gpio.h"
#include <stdint.h>

/* I2C адрес DAC7571 (8-бит, R/W=0 для записи) */
#define DAC7571_ADDRESS    0x98  /* 7-бит адрес 0x4C, сдвинутый влево */

/* Режимы работы DAC7571 (биты 15:14 в формате данных) */
#define DAC7571_MODE_NORMAL    0x00  /* Нормальный режим (активный вывод) */
#define DAC7571_MODE_PD_1K     0x01  /* Power-Down: 1 кОм на GND */
#define DAC7571_MODE_PD_100K   0x02  /* Power-Down: 100 кОм на GND */
#define DAC7571_MODE_PD_HIZ    0x03  /* Power-Down: High Impedance (Hi-Z) */

/* Управление питанием DAC (PD0 на GPIOD) */
#define DAC_PWR_PORT    GPIOD
#define DAC_PWR_PIN     GPIO_Pin_0

void dac_pwrpin_init(void);        /* Инициализация GPIO для пина питания */
void dac_pwrpin_on(void);          /* Включить питание DAC */
void dac_pwrpin_off(void);         /* Выключить питание DAC */

void dac_set_value(uint16_t data); /* Установить 12-бит значение (0x000..0xFFF) */
void dac_set_100K(void);           /* Power-Down: 100кОм на GND */

#endif /* DAC_H */
