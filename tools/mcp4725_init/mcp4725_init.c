// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file mcp4725_init.c
 * @brief Standalone service firmware to program MCP4725 EEPROM with 0x000 (0V).
 *
 * Out of the box from factory, MCP4725 EEPROM contains mid-scale (50% / 0x800).
 * Upon power-up via PD0, MCP4725 outputs 50% voltage during POR, causing
 * an unwanted LED flash.
 *
 * This utility:
 * 1. Drives PD0 high (powers on MCP4725).
 * 2. Inits I2C1 (400 kHz).
 * 3. Sends Write DAC Register and EEPROM command:
 *    [0x60 (Addr W)] [0x60 (Cmd: Write DAC & EEPROM)] [0x00 (MSB)] [0x00 (LSB)]
 * 4. Waits 50 ms for internal EEPROM write cycle.
 * 5. Visual status indication:
 *    - Success: Green LED (PC4) blinks 3 times, then stays ON.
 *    - Error (NACK / bus failure): Red LED (PC7) blinks continuously.
 * 6. Cuts DAC power (PD0 = 0) and halts.
 */

#include "ch32v00x.h"
#include "proteus_config.h"
#include "i2c.h"
#include "mini_gpio.h"

// Directly include i2c.c to build as a self-contained single translation unit
#include "../../src/i2c.c"

/** @brief Software delay in milliseconds. */
static void delay_ms(uint32_t ms)
{
    volatile uint32_t count = ms * (SYSCLK_HZ / 4000u);
    while (count--) {
        __NOP();
    }
}

int main(void)
{
    SystemInit();

    // 1. Enable peripheral clocks: GPIOC (LEDs), GPIOD (DAC power PD0)
    RCC->APB2PCENR |= (RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD);

    // 2. Configure LEDs (PC4 = Green, PC7 = Red high-side, PC6 = Red low-side)
    GPIOC->BCR = GPIO_Pin_4 | GPIO_Pin_7;
    GPIO_PinConfig(GPIOC, GPIO_Pin_4, CFG_OUT_PP_2M);
    GPIO_PinConfig(GPIOC, GPIO_Pin_7, CFG_OUT_PP_2M);
    GPIO_PinConfig(GPIOC, GPIO_Pin_6, CFG_IN_PULL);

    // 3. Configure DAC power pin PD0
    GPIOD->OUTDR &= ~GPIO_Pin_0;
    GPIO_PinConfig(GPIOD, GPIO_Pin_0, CFG_OUT_PP_2M);

    // 4. Power on DAC and wait for POR (~10 ms)
    GPIOD->BSHR = GPIO_Pin_0;
    delay_ms(10);

    // 5. Initialize I2C1 (400 kHz)
    i2c_init(400000u);

    // 6. Write DAC register & EEPROM:
    // Address: 0x60 (A0 tied to GND)
    // Command byte: 0x60 (C2=0, C1=1, C0=1 -> Write DAC and EEPROM, PD1=0, PD0=0 -> Normal mode)
    // MSB: 0x00 (D11..D4 = 0)
    // LSB: 0x00 (D3..D0 = 0, lower 4 bits don't care = 0)
    uint8_t err = 0;
    if (i2c_start() != I2C_OK) err = 1;
    if (!err && i2c_send_addr(MCP4725_I2C_ADDR, I2C_DIR_TX) != I2C_OK) err = 2;
    if (!err && i2c_write_byte(0x60) != I2C_OK) err = 3;
    if (!err && i2c_write_byte(0x00) != I2C_OK) err = 4;
    if (!err && i2c_write_byte(0x00) != I2C_OK) err = 5;
    if (!err && i2c_stop() != I2C_OK) err = 6;

    if (!err) {
        // Wait 50 ms for EEPROM programming cycle (tw max 50 ms)
        delay_ms(50);

        // Turn off DAC power
        GPIOD->BCR = GPIO_Pin_0;
        i2c_deinit();

        // Success: blink Green LED 3 times, then keep Green LED ON
        for (uint8_t i = 0; i < 3; i++) {
            GPIOC->BSHR = GPIO_Pin_4;
            delay_ms(200);
            GPIOC->BCR = GPIO_Pin_4;
            delay_ms(200);
        }
        GPIOC->BSHR = GPIO_Pin_4;
    } else {
        // Error: turn off DAC and blink Red LED repeatedly
        GPIOD->BCR = GPIO_Pin_0;
        i2c_deinit();
        while (1) {
            GPIOC->BSHR = GPIO_Pin_7;
            delay_ms(250);
            GPIOC->BCR = GPIO_Pin_7;
            delay_ms(250);
        }
    }

    // Halt
    while (1) {
        __WFI();
    }
}
