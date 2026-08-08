/**
 * @file button_gpio.c
 * @brief Button GPIO + EXTI interrupt handler
 *
 * Button: PC3 -> EXTI Line3 -> EXTI7_0_IRQHandler
 *
 * Initialisation (button_gpio_init):
 *   1. Clock GPIOC + AFIO
 *   2. PC3 = Input Pull-Up
 *   3. EXTI3 -> GPIOC
 *   4. EXTI Line3: falling edge interrupt
 *   5. NVIC: EXTI7_0_IRQn (IRQn=20)
 *
 * In ISR (EXTI7_0_IRQHandler):
 *   - Wait for UART TC (if debug enabled)
 *   - need_sleep = false
 *   - wakeup_source = WS_BUTTON
 *   - Clear EXTI3 flag
 */

#include "button_gpio.h"
#include "mini_gpio.h"
#include "proteus_config.h"
#include "awu.h"

/** @brief EXTI7_0 interrupt handler for button wake. */
void EXTI7_0_IRQHandler(void)
    __attribute__((interrupt("WCH-Interrupt-fast")));

void EXTI7_0_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line3) != RESET)
    {
        #ifdef UART_DEBUG
        while (!(USART1->STATR & USART_STATR_TC)) {}
        #endif

        need_sleep = false;
        wakeup_source = WS_BUTTON;
        
        EXTI_ClearITPendingBit(EXTI_Line3);
    }
}

/** @brief Initialise button GPIO, EXTI and NVIC. */
void button_gpio_init(void)
{
    RCC->APB2PCENR |= (1 << 4) | (1 << 0);

    GPIO_PinConfig(GPIOC, GPIO_Pin_3, CFG_IN_PULL);
    GPIOC->OUTDR |= GPIO_Pin_3;

    AFIO->EXTICR = 0x0080;

    EXTI->INTENR |= (1 << 3);
    EXTI->FTENR |= (1 << 3);
    EXTI->RTENR &= ~(1 << 3);

    NVIC->IENR[0] = (1 << 20);
}
