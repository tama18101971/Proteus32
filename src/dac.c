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
 *
 * Обработка ошибок:
 *   dac_write() проверяет ACK на каждом шаге (адрес, MSB, LSB) через
 *   i2c_send_addr() / i2c_write_byte(), которые сами генерируют STOP и
 *   освобождают шину при NACK. Если любой шаг не подтверждён — транзакция
 *   прерывается немедленно и наружу возвращается I2C_NACK, вместо того
 *   чтобы продолжать отправку в пустоту (например, если DAC не запитан
 *   или физически отсутствует на шине).
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
 *
 * Запись в BSHR (Bit Set Register) затрагивает только указанный бит —
 * в отличие от прямой записи в OUTDR, это атомарная операция, безопасная
 * без отдельного read-modify-write и не влияющая на другие пины порта.
 */
void dac_pwrpin_on(void) {
    DAC_PWR_PORT->BSHR = DAC_PWR_PIN;
}

/**
 * @brief Выключить питание DAC (PD0 = LOW)
 *
 * Запись в BCR (Bit Clear Register) — атомарный сброс только этого бита,
 * аналогично BSHR при включении.
 */
void dac_pwrpin_off(void) {
    DAC_PWR_PORT->BCR = DAC_PWR_PIN;
}

/**
 * @brief Отправка двух байтов в DAC7571 через I2C
 * @param msb — старший байт (режим + старшие 4 бита данных)
 * @param lsb — младший байт (младшие 8 бит данных)
 * @return I2C_OK (0) — все три шага подтверждены ACK,
 *         I2C_NACK (1) — DAC не ответил на адрес или на один из байт
 *         (например, питание выключено, DAC не подключён, обрыв шины).
 *
 * Формат I2C транзакции:
 *   START → [0x98] → [msb] → [lsb] → STOP
 *
 * Каждый шаг проверяется на ACK. При ошибке на любом шаге i2c_send_addr()
 * / i2c_write_byte() уже сами генерируют STOP и ждут освобождения шины,
 * поэтому дополнительный i2c_stop() в этом случае не нужен — но он
 * безопасен и используется в конце успешного пути.
 */
static uint8_t dac_write(uint8_t msb, uint8_t lsb) {
    i2c_start();

    if (i2c_send_addr(DAC7571_ADDRESS >> 1, I2C_DIR_TX) != I2C_OK) {
        return I2C_NACK;
    }
    if (i2c_write_byte(msb) != I2C_OK) {
        return I2C_NACK;
    }
    if (i2c_write_byte(lsb) != I2C_OK) {
        return I2C_NACK;
    }

    i2c_stop();
    return I2C_OK;
}

/**
 * @brief Установка 12-бит значения DAC (нормальный режим)
 * @param data — 12-бит значение (0x000..0xFFF)
 * @return I2C_OK (0) если DAC подтвердил приём, I2C_NACK (1) при ошибке
 *
 * Формирование формата данных:
 *   MSB = [режим(2 бита)] [00] [старшие 4 бита]
 *   LSB = [младшие 8 бит]
 *
 * Пример: data = 0xABC
 *   MSB = (0x00 << 4) | (0xA) = 0x0A
 *   LSB = 0xBC
 */
uint8_t dac_set_value(uint16_t data) {
    data &= 0x0FFF;
    uint8_t msb = (uint8_t)(((DAC7571_MODE_NORMAL << 4) & 0x30) | ((data >> 8) & 0x0F));
    uint8_t lsb = (uint8_t)(data & 0xFF);
    return dac_write(msb, lsb);
}

/**
 * @brief Перевод DAC в режим Power-Down 100кОм
 * @return I2C_OK (0) если DAC подтвердил приём, I2C_NACK (1) при ошибке
 *
 * DAC7571 переходит в режим пониженного энергопотребления.
 * Выход подключён к GND через резистор 100кОм.
 * Потребление: ~1мкА (вместо ~250мкА в нормальном режиме).
 */
uint8_t dac_set_100K(void) {
    uint8_t msb = (uint8_t)((DAC7571_MODE_PD_100K << 4) & 0x30);
    return dac_write(msb, 0x00);
}
