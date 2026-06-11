#include "awu.h"
#include <stdint.h>
#include <stdbool.h>
#include "ch32v00x_pwr.h"

bool aux2awu(awu_tick_t tick, uint32_t *Prescaler, uint8_t *WindowValue)
{
    switch (tick)
    {
    case AUX_15MS:
        *Prescaler = PWR_AWU_Prescaler_128;
        *WindowValue = 15;
        break;
    case AUX_30MS:
        *Prescaler = PWR_AWU_Prescaler_128;
        *WindowValue = 30;
        break;
    case AUX_60MS:
        *Prescaler = PWR_AWU_Prescaler_256;
        *WindowValue = 30;
        break;
    case AUX_120MS:
        *Prescaler = PWR_AWU_Prescaler_256;
        *WindowValue = 60;
        break;
    case AUX_250MS:
        *Prescaler = PWR_AWU_Prescaler_512;
        *WindowValue = 63;
        break;
    case AUX_500MS:
        *Prescaler = PWR_AWU_Prescaler_1024;
        *WindowValue = 63;
        break;
    case AUX_1S:
        *Prescaler = PWR_AWU_Prescaler_2048;
        *WindowValue = 63;
        break;
    case AUX_2S:
        *Prescaler = PWR_AWU_Prescaler_10240;
        *WindowValue = 25;
        break;
    case AUX_4S:
        *Prescaler = PWR_AWU_Prescaler_10240;
        *WindowValue = 50;
        break;
    case AUX_8S:
        *Prescaler = PWR_AWU_Prescaler_61440;
        *WindowValue = 17;
        break;
    case AUX_16S:
        *Prescaler = PWR_AWU_Prescaler_61440;
        *WindowValue = 33;
        break;
    default:
        return false;
        break;
    }
    return true;
}

void awu_set_delay(awu_tick_t awu_tick)
{

    uint8_t WindowValue;
    uint32_t Prescaler;

    if (aux2awu(awu_tick, &Prescaler, &WindowValue))
    {
        PWR_AWU_SetPrescaler(Prescaler);
        PWR_AWU_SetWindowValue(WindowValue);
    }
}

/*********************************************************************
 * EXTI initialization
 *********************************************************************/
void EXTI_INT_INIT(void)
{
    EXTI_InitTypeDef EXTI_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    EXTI_InitStructure.EXTI_Line = EXTI_Line9;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;

    EXTI_Init(&EXTI_InitStructure);
    
}

/*********************************************************************
 * AWU initialization
 *********************************************************************/
void AWU_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    RCC_LSICmd(ENABLE);

    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET)
        ;
}