#include "debug.h"
#include "awu.h"
#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include "led_gpio.h"
#include "i2c.h"
#include "dac.h"

typedef enum
{
    MODE_BEACON = 0,
    MODE_ON
} driver_mode_t;

typedef enum
{
    LED_OFF_STATE = 0,
    LED_ON_STATE
} led_state_t;

volatile uint8_t button_event = 0;

driver_mode_t mode = MODE_BEACON;
led_state_t led_state = LED_OFF_STATE;

/*********************************************************************
 * @fn      EXTI_INT_INIT
 *
 * @brief   Initializes EXTI collection.
 *
 * @return  none
 */
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
 * Button interrupt
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
 * Button PC3
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

/*********************************************************************
 * Sleep ~480 ms
 *********************************************************************/
void Sleep500ms(void)
{
    awu_set_delay(AUX_500MS);

    PWR_AutoWakeUpCmd(ENABLE);

    PWR_EnterSTANDBYMode(PWR_STANDBYEntry_WFE);

    PWR_AutoWakeUpCmd(DISABLE);

    SystemCoreClockUpdate();
    Delay_Init();

    #ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);
    #endif
}

/*********************************************************************
 * Sleep ~144 ms
 *********************************************************************/
void Sleep150ms(void)
{
    awu_set_delay(AUX_60MS);

    PWR_AutoWakeUpCmd(ENABLE);

    PWR_EnterSTANDBYMode(PWR_STANDBYEntry_WFE);

    PWR_AutoWakeUpCmd(DISABLE);

    SystemCoreClockUpdate();
    Delay_Init();

    #ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);
    #endif
}

/*********************************************************************
 * Main
 *********************************************************************/
int main(void)
{
    uint32_t idle_ms = 0;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    Delay_Ms(4000);

    EXTI_INT_INIT();

    #ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);
    #endif

    EE_PRINTF("Boot");

    led_init();
    Button_Init();
    AWU_init();

    I2C1_Init(I2C_SPEED_400K);
    DAC7571_Set100K();
    DAC7571_SetValue(1024);

    while (1)
    {
        if (mode == MODE_BEACON)
        {
            if (led_state == LED_OFF_STATE)
            {
                on_grn_l();
                on_red_l();

                led_state = LED_ON_STATE;

                EE_PRINTF("Sleep 150ms");
                //Sleep150ms();
                Delay_Ms(1250);
            }
            else
            {
                off_grn();
                off_red();

                led_state = LED_OFF_STATE;

                EE_PRINTF("Sleep 500ms");
                //Sleep500ms();
                Delay_Ms(1500);
            }
        }
        else
        {
            if (button_event)
            {
                button_event = 0;
                idle_ms = 0;

                EE_PRINTF("Button pressed");
            }

            /***************************************************
             * USER CODE
             ***************************************************/
            Delay_Ms(10);

            /***************************************************/

            idle_ms += 10;

            if (idle_ms >= 3000)
            {
                idle_ms = 0;
                mode = MODE_BEACON;

                EE_PRINTF("Return to standby mode");
            }
        }
    }
}