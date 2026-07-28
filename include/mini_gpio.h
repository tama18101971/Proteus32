#ifndef MINI_GPIO_H
#define MINI_GPIO_H

/*
 * mini_gpio.h — Утилиты для настройки GPIO на CH32V003
 *
 * Константы конфигурации пинов:
 *   CFG_OUT_PP_10M/2M/50M — выход Push-Pull с разной скоростью
 *   CFG_IN_PULL            — вход с pull-up/pull-down
 *   CFG_IN_FLOAT           — вход плавающий (High-Z)
 *   CFG_IN_ANALOG          — аналоговый вход
 *
 * Функции:
 *   GPIO_PinConfig() — настройка одного пина (порт + маска + конфигурация)
 *   PIN_NUM()        — вычисление номера пина из битовой маски
 *
 * Примечание: CH32V003 имеет один регистр CFGLR на порт (32 бита = 8 пинов × 4 бита)
 */

#include "ch32v00x.h"

// PIN_NUM macro
#define PIN_NUM(pin_mask) (__builtin_ctz(pin_mask))

// CFG constants
#define CFG_OUT_PP_10M   0x01  // Выход Push-Pull, 10 МГц
#define CFG_OUT_PP_2M    0x02  // Выход Push-Pull, 2 МГц
#define CFG_OUT_PP_50M   0x03  // Выход Push-Pull, 50 МГц
#define CFG_IN_PULL      0x08  // Вход с Pull-up / Pull-down
#define CFG_IN_FLOAT     0x04  // Вход плавающий
#define CFG_IN_ANALOG    0x00  // Аналоговый вход

// Inline function GPIO_PinConfig
static inline void GPIO_PinConfig(GPIO_TypeDef* port, uint16_t pin_mask, uint8_t cfg) {
    uint8_t pin = PIN_NUM(pin_mask);
    uint32_t shift = pin * 4;          // Сдвиг (0, 4, 8, 12, 16, 20, 24, 28)
    uint32_t mask = ~(0xF << shift);   // Маска для очистки этих 4 бит

    // В CH32V003 только один регистр конфигурации на порт
    port->CFGLR = (port->CFGLR & mask) | ((uint32_t)cfg << shift);
}

#endif // MINI_GPIO_H
