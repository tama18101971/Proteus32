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
#include "ui.h"
#include "adc.h"
#include "timers.h"

volatile wakeup_source_t wakeup_source = WS_NONE;

extern uint8_t is_sleep_mode; /* 0 - не во сне, 1 - во сне */
uint8_t need_sleep = false; /* флаг определяет необходимость перейти в режим сна */
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

    adc_init();

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

    // start_aux_cmd(ERRO_RED, true, IDM_BEACON, AWU_120MS);
    // start_aux_cmd(INFO_GRN, true, IDM_BEACON, AWU_120MS);
    // wakeup_source = WS_AWU;

    start_aux_array(buf_test_blink, AWU_120MS);
    need_sleep = true;

    /* Интервалы таймеров (аналог millis в Arduino) */

    while (1)
    {
        wakeup_source_t ws = WS_NONE;

        if (need_sleep)   /* если сон нужен, то и останавливаем перифирию и затем спим */
        {   
            //need_sleep = false;
            adc_stop();
            tim2_stop();
            ws = AWU_Sleep_Now();
        }

        if ((ws == WS_BUTTON) && is_sleep_mode) /* проснулись по кнопке т.к. is_sleep_mode == true значит кнопка была нажата ранее в режиме сна */
        {
            is_sleep_mode = false;
            ws = WS_NONE;
            adc_init();
            tim2_start();
        }

        if (ws == WS_AWU) /* handle_aux() вызывается только при срабатывании AWU. Если AWU не сработало значит заданий нет */
        {
            ws = WS_NONE;
            handle_aux();
        }

        /* 150 Гц (каждые ~7 мс) */
        if (tick_150hz())
        {
            scan_ui1();
            ss_scan();
        }

        /* 60 Гц (каждые ~17 мс) */
        if (tick_60hz())
        {
            adc_read();
        }

        /* 5 секунд */
        if (tick_5s())
        {
            hanlde_ee();
        }
    }
}
