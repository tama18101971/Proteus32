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

volatile wakeup_source_t wakeup_source = WS_NONE;
volatile uint8_t awu_event = 0;
volatile uint8_t button_event = 0;

uint8_t	need_pwd = false;	
extern uint8_t need_save;

int main(void)
{

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
    AWU_set_delay(AWU_15MS);

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

    // start_aux_cmd(ERRO_RED, true, IDM_BEACON);
    // start_aux_cmd(INFO_GRN, true, IDM_BEACON);
    wakeup_source = WS_AWU;

    start_aux_array(buf_test_blink);

    while (1)
    {
        if (wakeup_source == WS_AWU)
        {
            wakeup_source = WS_NONE;
            handle_aux();
            AWU_Sleep_Now();
        }

        ntc_read();
        scan_ui1();
        ss_scan();
        hanlde_ee();
    }
}
