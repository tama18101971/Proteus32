/*
 * dac.c — Драйвер ЦАП DAC7571 (I2C, 12-бит)
 *
 * DAC7571 подключён по I2C1 (PC1=SDA, PC2=SCL).
 * Используется для управления яркостью светодиодов (12-бит, 4096 уровней).
 *
 * Протокол передачи:
 *   1. i2c_start()
 *   2. Отправка адреса (0x4C << 1 | 0 = 0x98)
 *   3. MSB: [режим(2 бита)] [00] [старшие 4 бита данных]
 *   4. LSB: [младшие 8 бит данных]
 *   5. i2c_stop()
 *
 * Управление питанием:
 *   PD0 (GPIOD) — включение/отключение питания DAC
 *   При выключении питания DAC переходит в Hi-Z (экономия тока)
 */

#include "dac.h"
#include "i2c.h"
#include "ch32v00x.h"
#include "config.h"

/**
 * @brief Инициализация GPIO для пина питания DAC (PD0)
 *
 * Настраивает PD0 как Push-Pull output 2MHz.
 * Изначально питание выключено (PD0 = LOW).
 */
void dac_pwrpin_init(void) {
    RCC->APB2PCENR |= (1 << 5);  /* GPIOD clock enable (bit 5) */
    DAC_PWR_PORT->OUTDR &= ~DAC_PWR_PIN;
    GPIO_PinConfig(DAC_PWR_PORT, DAC_PWR_PIN, CFG_OUT_PP_2M);
}

/**
 * @brief Включить питание DAC (PD0 = HIGH)
 */
void dac_pwrpin_on(void) {
    DAC_PWR_PORT->BSHR = DAC_PWR_PIN;
}

/**
 * @brief Выключить питание DAC (PD0 = LOW)
 */
void dac_pwrpin_off(void) {
    DAC_PWR_PORT->BCR = DAC_PWR_PIN;
}

/**
 * @brief Отправка двух байтов в DAC7571 через I2C
 * @param msb — старший байт (режим + старшие 4 бита данных)
 * @param lsb — младший байт (младшие 8 бит данных)
 *
 * Формат I2C транзакции:
 *   START → [0x98] → [msb] → [lsb] → STOP
 */
static void dac_write(uint8_t msb, uint8_t lsb) {
    i2c_start();
    i2c_send_addr(DAC7571_ADDRESS >> 1, I2C_DIR_TX);
    i2c_send_byte(msb);
    i2c_send_byte(lsb);
    i2c_stop();
}

/**
 * @brief Установка 12-бит значения DAC (нормальный режим)
 * @param data — 12-бит значение (0x000..0xFFF)
 *
 * Формирование формата данных:
 *   MSB = [режим(2 бита)] [00] [старшие 4 бита]
 *   LSB = [младшие 8 бит]
 *
 * Пример: data = 0xABC
 *   MSB = (0x00 << 4) | (0xA) = 0x0A
 *   LSB = 0xBC
 */
void dac_set_value(uint16_t data) {
    data &= 0x0FFF;
    uint8_t msb = (uint8_t)(((DAC7571_MODE_NORMAL << 4) & 0x30) | ((data >> 8) & 0x0F));
    uint8_t lsb = (uint8_t)(data & 0xFF);
    dac_write(msb, lsb);
}

/**
 * @brief Перевод DAC в режим Power-Down 100кОм
 *
 * DAC7571 переходит в режим пониженного энергопотребления.
 * Выход подключён к GND через резистор 100кОм.
 * Потребление: ~1мкА (вместо ~250мкА в нормальном режиме).
 */
void dac_set_100K(void) {
    uint8_t msb = (uint8_t)((DAC7571_MODE_PD_100K << 4) & 0x30);
    dac_write(msb, 0x00);
}
