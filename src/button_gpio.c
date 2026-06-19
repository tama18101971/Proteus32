#include "button_gpio.h"
#include "ch32v003_gpio.h"
#include "awu.h"
#include "config.h"

/*********************************************************************
 * Button interrupt handler
 *********************************************************************/
void EXTI7_0_IRQHandler(void)
    __attribute__((interrupt("WCH-Interrupt-fast")));

void EXTI7_0_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line3) != RESET)
    {
        EE_PRINTF("[BG] WAKEUP Source: BUTTON PRESS\r\n");
        #ifdef WL_FLASH_DEBUG
        /* Ждём завершения передачи UART */
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {}
        #endif

        button_event = 1;
        mode= MODE_ON1;
        
        AWU_set_delay(AWU_15MS);

        EXTI_ClearITPendingBit(EXTI_Line3);
    }
}

/*********************************************************************
 * Button initialization (GPIO + EXTI + NVIC)
 *********************************************************************/
void button_init(void)
{
    // 1. Включаем тактирование GPIOC и AFIO
    RCC->APB2PCENR |= (1 << 4) | (1 << 0);  // RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO

    // 2. Настраиваем PC3 как вход с подтяжкой (Input Pull-Up)
    GPIO_PinConfig(GPIOC, GPIO_Pin_3, CFG_IN_PULL);
    GPIOC->OUTDR |= GPIO_Pin_3;  // подтяжка вверх (Pull-Up)

    // 3. Маппим EXTI3 на GPIOC (AFIO_EXTICR1_EXTI3_PC = 0x0080)
    AFIO->EXTICR = 0x0080;

    // 4. Настраиваем EXTI Line3: прерывание по спадающему фронту
    EXTI->INTENR |= (1 << 3);   // разрешаем прерывание EXTI3
    EXTI->FTENR |= (1 << 3);    // по спадающему фронту (Falling Edge)
    EXTI->RTENR &= ~(1 << 3);   // запрещаем по нарастающему

    // 5. Включаем NVIC для EXTI7_0_IRQn (IRQn = 20)
    NVIC->IENR[0] = (1 << 20);
}