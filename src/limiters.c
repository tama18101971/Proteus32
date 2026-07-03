#include "adc.h"
#include "ui.h"
#include "pi_reg.h"
#include "limiters.h"
#include "wl_flash.h"
#include "config.h"


/* ============ СТРУКТУРЫ РЕГУЛЯТОРА ============ */

static pi_reg_config_t led_thermal_config;
pi_reg_state_t led_thermal_state;

uint16_t limiter = 0;

void sys_init(void)
{
led_thermal_config.setpoint = 375;      // Уставка 750 (10-бит)
led_thermal_config.start_offset = 60;   // Работаем в зоне от 720 до 750
led_thermal_config.predict_distance = 80; // FF начинает работать за 120 кодов до уставки

led_thermal_config.kp = 80;             // Ослабляем пропорциональный пинок (0.125)
led_thermal_config.ki = 1;              // Тормозим интегратор в 4 раза! (0.0078)
led_thermal_config.k_predict = 128;     // Оставляем как есть, он гасит первичный вылет

led_thermal_config.max_i_term = 2000;
led_thermal_config.minimum_pwm = 50;
led_thermal_config.filter_shift = 3;
led_thermal_config.slew_rise = 1;
led_thermal_config.slew_release = 10;

pi_reg_init(&led_thermal_config, &led_thermal_state, raw_temperature_adc);
}

void scan_limiters(void)
{
#define MIN(a, b) ((a < b) ? (a) : (b))

    static uint8_t old_correction;

    if (driver_mode < MODE_ON)
        return;

    limiter = pi_reg_compute(&led_thermal_config, &led_thermal_state, raw_temperature_adc, step_user);


    if (old_correction != limiter)
    {
        uint16_t up_level = MAX_STEP - limiter;
        set_current_ll(MIN(up_level, step_user), SS_NO); // обновляем только когда включено для того,  чтобы не зажечь диод когда этого не надо.

        /*         if ((ee.uif.ui_index > 1) && (mode_current == CMODE_TURBO)) // если UI > 1 значит включены дискретные UI, если режим Турбо
                {
                    if (up_level < get_stepi(CMODE_MAX)) // Видим, что ТК увел яркость ниже максимального уровня и выключаем турбо.
                    {
                        mode_current = CMODE_MAX;
                        ps.use_prev = false; // отключем возврат по двойному клику из Турбо. 12.10.2021
                    }
                }
         */
        old_correction = limiter;
    }
}

void reset_limiters(void)
{
    pi_reg_reset(&led_thermal_config, &led_thermal_state, raw_temperature_adc);
}