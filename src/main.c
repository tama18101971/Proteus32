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

volatile uint8_t button_event = 0;
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

#ifdef SLEEP_STANDBY
    AWU_init();
    AWU_int_init();
#endif

    led_init();
    button_init();

    /* DAC */
    dac_pwrpin_init();
    dac_pwrpin_on();

    i2c_init(I2C_SPEED_400K);

    dac_set_value(1024);
    /* DAC */

    if (read_ee() == FAILED)
    {
        init_ee();
    }

#ifdef SLEEP_STANDBY
    AWU_set_delay(AWU_15MS);
    // здесь дописать усыпание

#endif

    // start_aux_cmd(ERRO_RED, AWU_15MS, true, IDM_BEACON);
    start_aux_cmd(INFO_GRN, AWU_15MS, true, IDM_BEACON);

    start_aux_array(buf_sos_red);
    // start_aux_array(buf_grn);

    while (1)
    {
        if (mode == MODE_BEACON)
        {
#ifdef SLEEP_STANDBY
            AWU_set_delay(AWU_15MS);
#endif

            handle_aux();
            scan_ui1();

#ifdef SLEEP_STANDBY
            AWU_Sleep_Now();
#else
            Delay_Ms(15);
#endif
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
