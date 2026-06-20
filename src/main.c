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
#include "ntc_table.h"
#include "timers.h"

volatile wakeup_source_t wakeup_source = WS_NONE;

uint8_t need_pwd = false;
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

/* Интервалы таймеров (аналог millis в Arduino) */
#define TICK_150HZ_MS 7 /* 1000мс / 150Гц ≈ 6.67мс → округляем до 7мс */
#define TICK_60HZ_MS 17 /* 1000мс / 60Гц ≈ 16.67мс → округляем до 17мс */
#define TICK_5S_MS 5000 /* 5 секунд */

    uint32_t last_tick_150hz = 0;
    uint32_t last_tick_60hz = 0;
    uint32_t last_tick_5s = 0;

    //tim2_start();

    while (1)
    {
        if (driver_mode > MODE_OFF) // здесь кода включено и не до сна
        {
            tim2_start();
            
            /* 150 Гц (каждые ~7 мс) */
            if (tim2_ms - last_tick_150hz >= TICK_150HZ_MS)
            {
                last_tick_150hz += TICK_150HZ_MS;
                scan_ui1();
                ss_scan();
            }

            /* 60 Гц (каждые ~17 мс) */
            if (tim2_ms - last_tick_60hz >= TICK_60HZ_MS)
            {
                last_tick_60hz += TICK_60HZ_MS;
                ntc_read();
            }

            /* 5 секунд */
            if (tim2_ms - last_tick_5s >= TICK_5S_MS)
            {
                // сброс на начало: last_tick_5s = tim2_ms;
                last_tick_5s += TICK_5S_MS;
                hanlde_ee();
            }

            if (wakeup_source == WS_AWU)
            {
                wakeup_source = WS_NONE;
                handle_aux();
                AWU_Sleep_Now();
            }
        }
        else // здесь сонные режимы
        {
            tim2_stop();
            handle_aux();
            AWU_Sleep_Now();
            //wakeup_source = WS_NONE;
        }
    }
}
