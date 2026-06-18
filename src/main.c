#include "debug.h"
#include "awu.h"
#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include "led_gpio.h"
#include "i2c.h"
#include "dac.h"
#include "button_gpio.h"
#include "aux_leds.h"
#include "wl_flash.h"
#include "button.h"
#include "ntc_table.h"

volatile uint8_t button_event = 0;
extern uint8_t need_save;
volatile driver_mode_t mode = MODE_BEACON;

led_state_t led_state = LED_OFF_STATE;

int main(void)
{
    uint32_t idle_ms = 0;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    Delay_Ms(3000);

#ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);
#endif

    EE_PRINTF("\r\n[MAIN] Boot...\r\n");

    AWU_init();
    AWU_int_init();

    led_init();
    button_init();

    ntc_adc_Init();

    /* DAC */
    dac_pwrpin_init();
    dac_pwrpin_on();

    i2c_init(I2C_SPEED_400K);

     dac_set_value(0);
    /* DAC */

    if (read_ee() == FAILED)
    {
        init_ee();
    }

    //start_aux_cmd(ERRO_RED, true, IDM_BEACON);
    //start_aux_cmd(INFO_GRN, true, IDM_BEACON);

    start_aux_array(buf_test_blink);

    while (1)
    {
        if (mode == MODE_BEACON)
        {
            ntc_read();

            handle_aux();
            scan_ui1();
            ss_scan();

            if (need_save)
            {
                write_ee();
                need_save = false;
            }

            AWU_Sleep_Now();
        }
        else
        {
            if (button_event)
            {
                button_event = 0;
                idle_ms = 0;

                EE_PRINTF("[MAIN] Button pressed\r\n");
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

                EE_PRINTF("[MAIN] Return to standby mode\r\n");
            }
        }
    }
}
