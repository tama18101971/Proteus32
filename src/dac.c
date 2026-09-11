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
 * Error handling: every step checks ACK/NACK. On any failure (DAC unpowered
 * or missing, bus error, timeout) the transaction aborts and returns
 * I2C_NACK. A timeout can leave the I2C block stuck mid-transfer, so on
 * failure the peripheral is reinitialised once (i2c_init(): SWRST +
 * reconfiguration) and the value cache stays invalid — the next write
 * retries the transaction on the recovered bus.
 */

#include "dac.h"
#include "i2c.h"
#include "mini_gpio.h"

/* 0xFFFF is outside the 12-bit DAC range and represents an unknown register
 * value after power loss, reset or an unsuccessful I2C transaction. */
static uint16_t last_voltage = 0xFFFFu;

/**
 * @brief Инициализация GPIO для пина питания DAC (PD0)
 *
 * Настраивает PD0 как Push-Pull output 2MHz.
 * Изначально питание выключено (PD0 = LOW).
 */
void dac_pwrpin_init(void)
{
    last_voltage = 0xFFFFu;
    RCC->APB2PCENR |= (1 << 5); /* GPIOD clock enable (bit 5) */
    DAC_PWR_PORT->OUTDR &= ~DAC_PWR_PIN;
    GPIO_PinConfig(DAC_PWR_PORT, DAC_PWR_PIN, CFG_OUT_PP_2M);
}

/**
 * @brief Restore the DAC power-control pin after an AWU/STANDBY wake.
 *
 * Re-enables GPIOD and drives PD0 high without the low pulse performed by
 * dac_pwrpin_init().  This preserves DAC supply during an AWU-driven strobe.
 *
 * The value cache is invalidated unconditionally: in STANDBY PD0 is Hi-Z, so
 * whether the external DAC rail survives is hardware-dependent (needs scope
 * verification).  If the DAC7571 lost power (POR -> register 0), a stale cache
 * would skip the rewrite and the beam would stay dark until the next
 * brightness change.  The cost when the rail survives is one redundant I2C
 * write per wake (~100 µs) — negligible against AWU intervals.
 */
void dac_pwrpin_restore(void)
{
    last_voltage = 0xFFFFu;

    RCC->APB2PCENR |= (1 << 5); /* GPIOD clock enable (bit 5) */

    /* Keep the output latch high before switching PD0 back to output mode.
     * A strobe wake must not create a low pulse that power-cycles the DAC. */
    DAC_PWR_PORT->BSHR = DAC_PWR_PIN;
    GPIO_PinConfig(DAC_PWR_PORT, DAC_PWR_PIN, CFG_OUT_PP_2M);
    DAC_PWR_PORT->BSHR = DAC_PWR_PIN;
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
    last_voltage = 0xFFFFu;
    DAC_PWR_PORT->BCR = DAC_PWR_PIN;
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

    if (data_12bit == last_voltage)
        return I2C_OK;

    /* Do not retain a value until the complete transaction, including STOP,
     * has succeeded. Any early return therefore leaves the cache invalid. */
    last_voltage = 0xFFFFu;

    // 2. Формируем два байта по спецификации TI
    // Старший байт: PD1=0, PD0=0 (Normal Mode), далее 4 старших бита данных
    uint8_t byte_msb = (uint8_t)((data_12bit >> 8) & 0x0F);
    // Младший байт: оставшиеся 8 бит данных
    uint8_t byte_lsb = (uint8_t)(data_12bit & 0xFF);

    // 3. Низкоуровневая транзакция с пошаговым контролем ошибок
    if (i2c_start() != I2C_OK ||
        i2c_send_addr(DAC7571_I2C_ADDR, I2C_DIR_TX) != I2C_OK ||
        i2c_write_byte(byte_msb) != I2C_OK ||
        i2c_write_byte(byte_lsb) != I2C_OK ||
        i2c_stop() != I2C_OK)
    {
        /* Таймаут может оставить I2C в середине передачи: последующие
         * i2c_start() падали бы по таймауту до ближайшего i2c_init()
         * (яркость «залипала»). Восстанавливаем периферию однократно
         * (SWRST + перенастройка); кэш уже инвалидирован, следующая
         * запись повторит транзакцию по восстановленной шине. */
        i2c_init(0u);
        return I2C_NACK;
    }

    last_voltage = data_12bit;
    return I2C_OK;
}