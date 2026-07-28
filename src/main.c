/*
 * main.c — Точка входа и главный цикл проекта Proteus003
 *
 * Управление фонариком на базе CH32V003:
 * - 2 канала LED (красный, зелёный) через DAC
 * - NTC-датчик температуры (ADC)
 * - Кнопка управления (EXTI)
 * - AWU (Auto Wake-Up) для периодических задач и сна
 * - TIM2 (1мс системный тик) для таймерных интервалов
 * - EEPROM-эмуляция во Flash (настройки)
 *
 * Главный цикл:
 * 1. Если need_sleep — останавливаем ADC и TIM2, засыпаем в STANDBY
 * 2. При пробуждении по AWU — обрабатываем AUX-команды
 * 3. При пробуждении по кнопке — восстанавливаем периферию
 * 4. По таймерам: 150Гц (UI + softstart), 60Гц (ADC), 5с (EEPROM)
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

int16_t error; /* Текущая ошибка (filtered - setpoint) */

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

    start_aux_beacon();

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
            EE_PRINTF("T=%u F=%d I=%d L=%d P=%u\r\n",
                      raw_temperature_adc, state.filtered_temp,
                      state.integral, state.prev_limiter, step_user);
        }

        if (tick_5s())
        {
            handle_ee();
            need_sleep = true;
        }
    }
}
