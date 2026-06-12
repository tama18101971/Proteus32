#include "debug.h"
#include "awu.h"
#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include "led_gpio.h"
#include "i2c.h"
#include "dac.h"
#include "button.h"
#include "aux_leds.h"
#include "wl_flash.h"

volatile uint8_t button_event = 0;
volatile driver_mode_t mode = MODE_BEACON;

 led_state_t led_state = LED_OFF_STATE;

//union AUX_Cmd_u *p_red;
//union AUX_Cmd_u *p_grn;

/*********************************************************************
 * Sleep ~480 ms
 *********************************************************************/
void Sleep500ms(void)
{
    PWR_AWU_SetPrescaler(PWR_AWU_Prescaler_10240);
    PWR_AWU_SetWindowValue(6);

    PWR_AutoWakeUpCmd(ENABLE);

    PWR_EnterSTANDBYMode(PWR_STANDBYEntry_WFE);

    PWR_AutoWakeUpCmd(DISABLE);

    SystemInit();

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
    PWR_AWU_SetPrescaler(PWR_AWU_Prescaler_2048);
    PWR_AWU_SetWindowValue(9);

    PWR_AutoWakeUpCmd(ENABLE);

    PWR_EnterSTANDBYMode(PWR_STANDBYEntry_WFE);

    PWR_AutoWakeUpCmd(DISABLE);

    SystemInit();

    SystemCoreClockUpdate();
    Delay_Init();

    #ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);
    #endif
}

int main(void)
{
    uint32_t idle_ms = 0;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    Delay_Ms(4000);

    #ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);
    #endif

    EE_PRINTF("\r\nBoot...\r\n");

    EXTI_INT_INIT();

    led_init();
    Button_Init();
    AWU_init();

    I2C1_Init(I2C_SPEED_400K);
    DAC7571_Set100K();
    DAC7571_SetValue(1024);

    if (read_ee() == FAILED) {
        init_ee();
        }

    awu_set_delay(AUX_15MS);

    // здесь дописать усыпание

    test_aux_beacon();

    p_red = &aux_cmd_red;
	p_grn = &aux_cmd_grn;

	aux_ctl_red.enable = true;
	aux_ctl_grn.enable = true;

    while (1)
    {
        if (mode == MODE_BEACON)
        {
     		handle_aux();

            //Sleep500ms();
            Delay_Ms(15);
        }
        else
        {
            if (button_event)
            {
                button_event = 0;
                idle_ms = 0;

                EE_PRINTF("Button pressed\r\n");
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

                EE_PRINTF("Return to standby mode\r\n");
            }
        }
    }
}
