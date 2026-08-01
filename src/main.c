/**
 * @file main.c
 * @brief Entry point and main loop for Proteus003 firmware
 *
 * CH32V003-based flashlight with:
 * - Red/green AUX LEDs via GPIO
 * - NTC temperature sensing (ADC)
 * - Button control (EXTI)
 * - AWU (Auto Wake-Up) for periodic tasks and sleep
 * - TIM2 (1 ms system tick)
 * - EEPROM emulation in Flash (settings)
 *
 * Main loop:
 * 1. If need_sleep -> stop ADC + TIM2, enter STANDBY
 * 2. AWU wake -> handle AUX commands
 * 3. Button wake -> restore peripherals
 * 4. Timer tasks: 160 Hz (UI + softstart), 30 Hz (ADC + limiters), 2 Hz (debug), 5 s (EEPROM)
 */
#include <stdint.h>
#include <stdbool.h>
#include "led_gpio.h"
#include "i2c.h"
#include "dac.h"
#include "button_gpio.h"
#include "button.h"
#include "aux_leds.h"
#include "app_data.h"
#include "ui.h"
#include "adc.h"
#include "timers.h"
#include "limiters.h"
#include "ob.h"
#include "array.h"
#include "mini_printf.h"

volatile wakeup_source_t wakeup_source = WS_NONE; /* Источник последнего пробуждения */

uint8_t need_sleep = false; /* Флаг необходимости перейти в режим сна */

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    Delay_Ms(3000);

#ifdef WL_FLASH_DEBUG
    uart_init(460800);
#endif

    EE_PRINTF("\r\n[MAIN] Boot...\r\n");

    sys_init();

    AWU_init();
    AWU_int_init();
    AWU_set_delay(AWU_15MS);

    led_init();
    button_gpio_init();

    adc_init();

    /* DAC + I2C */
    dac_pwrpin_init();
    dac_pwrpin_on();
    i2c_init(400000);
    dac7571_set_voltage(0);

    ob_Init();
    if (!read_ee())
    {
        init_ee();
        ee_defaults();
    }

    /* Инициализация FSM кнопки и UI */
    static const btn_config_t btn_cfg = {
        .hold_ticks = BTN_HOLD_TICKS,
        .repeat_ticks = BTN_REPEAT_TICKS,
        .release_ticks = BTN_RELEASE_TICKS,
        .max_clicks = BTN_MAX_CLICKS,
        .long_click_ticks = BTN_LONG_CLICK_TICKS,
    };
    button_init(&btn_cfg);
    ui_init();

    //start_aux_beacon();

    need_sleep = true;

    reset_limiters();

    EE_PRINTF("setpoint=%u kp_shift=%u ki_shift=%u ki_decim=%u offset=%u\r\n",
              BASE_TEMPERATURE + EE.usr.t_calibr, config.kp_left_shift, config.ki_left_shift,
              config.ki_decimation_shift, config.t_anticipate_offset);

    while (1)
    {
        /* 1. Сон: останавливаем ADC и TIM2, засыпаем в STANDBY */
        if (need_sleep)
        {
            adc_stop();
            tim2_stop();
            start_aux_beacon();
            AWU_Sleep_Now();
        }

        /* 2. Пробуждение по кнопке: восстанавливаем периферию */
        if ((wakeup_source == WS_BUTTON) && is_sleep_mode)
        {
            is_sleep_mode = false;
            need_sleep = false;
            wakeup_source = WS_NONE;
            stop_aux_beacon(false);
            adc_init();
            tim2_start();
            dac_pwrpin_init();
            i2c_init(400000);
        }

        /* 3. Пробуждение по AWU: обрабатываем AUX-команды */
        if (wakeup_source == WS_AWU)
        {
            wakeup_source = WS_NONE;
            handle_aux();
        }

        /* 4. Таймерные задачи */
        if (tick_160hz())
        {
            button_tick();
            ui_process();
            ss_scan();
        }

        if (tick_30hz())
        {
            adc_read();
            scan_limiters();
            scan_voltage();
        }

        if (tick_2hz())
        {
#ifdef WL_FLASH_DEBUG
            uint16_t pwm_out = (step_user > limiter) ? (step_user - limiter) : 0;
#endif
            EE_PRINTF("TEMP=%u TEMPF=%d I=%d LIM=%d PWM=%u\r\n STU=%u\r\n",
                      raw_temperature_adc, state.filtered_temp,
                      state.integral, state.prev_limiter, pwm_out, step_user);
        }

        if (tick_5s())
        {
            if(need_save) {
                ob_Flush();             // временно - потом переделать!!!
                handle_ee();
            }
            need_sleep = true;
        }
    }
}
