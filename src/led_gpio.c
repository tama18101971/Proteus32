#include "led_gpio.h"
#include "ch32v00x.h"

#include <stdint.h>

// Вспомогательный макрос для получения номера пина из маски
#define PIN_NUM(pin_mask) (__builtin_ctz(pin_mask))

// Универсальная функция настройки пина СПЕЦИАЛЬНО ДЛЯ CH32V003
static inline void GPIO_PinConfig(GPIO_TypeDef* port, uint16_t pin_mask, uint8_t cfg) {
    uint8_t pin = PIN_NUM(pin_mask);
    uint32_t shift = pin * 4;          // Сдвиг (0, 4, 8, 12, 16, 20, 24, 28)
    uint32_t mask = ~(0xF << shift);   // Маска для очистки этих 4 бит
    
    // В CH32V003 только один регистр конфигурации на порт
    port->CFGLR = (port->CFGLR & mask) | ((uint32_t)cfg << shift);
}

/*********************************************************************
 * LED PC4, PC7 - выход. PS6 - вход
 *********************************************************************/
void led_init(void)
{
    // 1. Включаем тактирование GPIOC
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC;

    // 2. Предустанавливаем нули в OUTDR (антиглитч!)
    // Используем макросы, чтобы код был независим от конкретных портов/пинов
    REDH_PORT->BCR = REDH_PIN;
    GRN_PORT->BCR = GRN_PIN;

    // 3. Настраиваем пины как выходы
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);

    // 4. Настраиваем Pin6 как плавающий вход
    GPIO_PinConfig(REDL_PORT, REDL_PIN, CFG_IN_FLOAT);
}

// Включить красный (на REDH выдается 1, на REDL выдается 0)
void on_red_h(void) {
    // Отключаем дополнительную подтяжку ВВЕРХ
    REDL_PORT->BCR = REDL_PIN;

    // Настраиваем REDH (Pin 7) как выход Push-Pull 10MHz
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
    // Включаем REDH (выдаем 1)
    REDH_PORT->BSHR = REDH_PIN;
}

// Включить подтяжку на красном
void on_red_l(void) {
    // Настраиваем REDH (Pin 7) как Вход с Pull-up/Pull-down (MODE=00, CNF=10)
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_IN_PULL);
	// Включаем подтяжку ВВЕРХ (запись в BSHR устанавливает бит в OUTDR, что дает Pull-up)
    REDH_PORT->BSHR = REDH_PIN;

	// Дополнительная подтяжка для выравнивания яркости
    REDL_PORT->BSHR = REDL_PIN;
}

// Выключить красный
void off_red(void) {
    // Отключаем дополнительную подтяжку ВВЕРХ
    REDL_PORT->BCR = REDL_PIN;

    // 1. Устанавливаем будущий уровень выхода в 0 (сбрасываем бит в OUTDR)
    REDH_PORT->BCR = REDH_PIN;
    // 2. Переключаем пин в режим выхода Push-Pull 10MHz
    GPIO_PinConfig(REDH_PORT, REDH_PIN, CFG_OUT_PP_10M);
}

// Включить зеленый
void on_grn_h(void) {
    // Настраиваем GRN (Pin 4) как выход Push-Pull 10MHz
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
    // Выдаем логическую 1 (запись в младшие биты BSHR устанавливает бит в OUTDR)
    GRN_PORT->BSHR = GRN_PIN; 
}

// Включить подтяжку на зеленом
void on_grn_l(void) {
    // Настраиваем GRN (Pin 4) как Вход с Pull-up/Pull-down (MODE=00, CNF=10)
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_IN_PULL);
    // Включаем подтяжку ВВЕРХ
    GRN_PORT->BSHR = GRN_PIN;
}

// Выключить зеленый
void off_grn(void) {
    // 1. Устанавливаем будущий уровень выхода в 0 (сбрасываем бит в OUTDR)
    GRN_PORT->BCR = GRN_PIN;
    // 2. Переключаем пин в режим выхода Push-Pull 10MHz
    GPIO_PinConfig(GRN_PORT, GRN_PIN, CFG_OUT_PP_10M);
}
