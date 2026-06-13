#include "button_gpio.h"
#include "ch32v00x.h"

/*********************************************************************
 * Button interrupt handler
 *********************************************************************/
void EXTI7_0_IRQHandler(void)
__attribute__((interrupt("WCH-Interrupt-fast")));

void EXTI7_0_IRQHandler(void)
{
    if(EXTI_GetITStatus(EXTI_Line3) != RESET)
    {
        button_event = 1;

        mode = MODE_ON;

        EXTI_ClearITPendingBit(EXTI_Line3);
    }
}

/*********************************************************************
 * Button initialization (GPIO + EXTI + NVIC)
 *********************************************************************/
void Button_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    EXTI_InitTypeDef EXTI_InitStructure = {0};
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO,  ENABLE);

    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;

    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_EXTILineConfig(GPIO_PortSourceGPIOC, GPIO_PinSource3);

    EXTI_InitStructure.EXTI_Line    = EXTI_Line3;
    EXTI_InitStructure.EXTI_Mode    = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;

    EXTI_Init(&EXTI_InitStructure);

    NVIC_InitStructure.NVIC_IRQChannel = EXTI7_0_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;

    NVIC_Init(&NVIC_InitStructure);
}