/**
 * @file dac.c
 * @brief DAC7571 I2C DAC driver
 *
 * DAC7571 connected via I2C1 (PC1=SDA, PC2=SCL).
 * 12-bit resolution for LED brightness control.
 *
 * Protocol:
 *   1. i2c_start()
 *   2. Send address (0x4C << 1 | 0 = 0x98)
 *   3. MSB: [mode(2b)] [00] [upper 4 data bits]
 *   4. LSB: [lower 8 data bits]
 *   5. i2c_stop()
 *
 * Power control: PD0 (GPIOD) switches DAC supply.
 * On power-off DAC enters Hi-Z (power saving).
 *
 * Error handling: dac_write() checks ACK on every step.
 * If any step gets NACK (DAC unpowered or missing), transaction
 * aborts immediately and returns I2C_NACK.
 */

#include "dac.h"
#include "i2c.h"
#include "mini_gpio.h"

/**
 * @brief Инициализация GPIO для пина питания DAC (PD0)
 *
 * Настраивает PD0 как Push-Pull output 2MHz.
 * Изначально питание выключено (PD0 = LOW).
 */
void dac_pwrpin_init(void)
{
    RCC->APB2PCENR |= (1 << 5); /* GPIOD clock enable (bit 5) */
    DAC_PWR_PORT->OUTDR &= ~DAC_PWR_PIN;
    GPIO_PinConfig(DAC_PWR_PORT, DAC_PWR_PIN, CFG_OUT_PP_2M);
}

/**
 * @brief Включить питание DAC (PD0 = HIGH)
 *
 * Запись в BSHR (Bit Set Register) затрагивает только указанный бит —
 * в отличие от прямой записи в OUTDR, это атомарная операция, безопасная
 * без отдельного read-modify-write и не влияющая на другие пины порта.
 */
void dac_pwrpin_on(void)
{
    DAC_PWR_PORT->BSHR = DAC_PWR_PIN;
}

/**
 * @brief Выключить питание DAC (PD0 = LOW)
 *
 * Запись в BCR (Bit Clear Register) — атомарный сброс только этого бита,
 * аналогично BSHR при включении.
 */
void dac_pwrpin_off(void)
{
    DAC_PWR_PORT->BCR = DAC_PWR_PIN;
}

/**
 * @brief Проверка состояния питания DAC
 * @return 1 если питание включено (PD0 = HIGH), 0 если выключено
 */
uint8_t dac_pwrpin_is_on(void)
{
    return (DAC_PWR_PORT->INDR & DAC_PWR_PIN) != 0;
}

/**
 * @brief Установка выходного напряжения на ЦАП DAC7571
 * @param data_12bit: значение от 0 до DAC_MAX_VALUE (12 бит)
 * @return I2C_OK или I2C_NACK
 */
uint8_t dac7571_set_voltage(uint16_t data_12bit)
{
    // 1. Защита от выхода за границы 12-битного разрешения
    if (data_12bit > DAC_MAX_VALUE)
    {
        data_12bit = DAC_MAX_VALUE;
    }

    // 2. Формируем два байта по спецификации TI
    // Старший байт: PD1=0, PD0=0 (Normal Mode), далее 4 старших бита данных
    uint8_t byte_msb = (uint8_t)((data_12bit >> 8) & 0x0F);
    // Младший байт: оставшиеся 8 бит данных
    uint8_t byte_lsb = (uint8_t)(data_12bit & 0xFF);

    // 3. Низкоуровневая транзакция с пошаговым контролем ошибок
    if (i2c_start() != I2C_OK)
        return I2C_NACK;

    if (i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX) != I2C_OK)
    {
        // Если устройство отключено, i2c_send_addr сам сгенерирует STOP
        return I2C_NACK;
    }

    // Передаем старший байт и ждем ACK
    if (i2c_write_byte(byte_msb) != I2C_OK)
        return I2C_NACK;

    // Передаем младший байт и ждем ACK
    if (i2c_write_byte(byte_lsb) != I2C_OK)
        return I2C_NACK;

    // Успешное завершение транзакции
    i2c_stop();
    return I2C_OK;
}

/**
 * @brief Перевод ЦАП в режим сна (Power-Down) для экономии энергии
 * @param mode: 1 - подтяжка 1 кОм к GND, 2 - 100 кОм к GND, 3 - High-Z (высокий импеданс)
 */
uint8_t dac7571_power_down(uint8_t mode)
{
    if (mode < 1 || mode > 3)
        return I2C_NACK;

    // Сдвигаем режим в биты PD1:PD0 (6-й и 7-й биты первого байта)
    uint8_t byte_msb = (mode << 6);

    if (i2c_start() != I2C_OK)
        return I2C_NACK;
    if (i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX) != I2C_OK)
        return I2C_NACK;
    if (i2c_write_byte(byte_msb) != I2C_OK)
        return I2C_NACK;
    if (i2c_write_byte(0x00) != I2C_OK)
        return I2C_NACK; // Второй байт не важен, но обязателен
    i2c_stop();

    return I2C_OK;
}