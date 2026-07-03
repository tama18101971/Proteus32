#include "adc.h"
#include "ui.h"
#include "pi_reg.h"
#include "limiters.h"

extern uint16_t raw_temperature_adc;
extern struct ee_data ee;

/* ============ СТРУКТУРЫ РЕГУЛЯТОРА ============ */

static pi_reg_config_t led_thermal_config;
static pi_reg_state_t  led_thermal_state;

uint16_t limiter = 0;

void sys_init(void) {
    // 1. Настройка конфигурации термоконтроля
    led_thermal_config.setpoint     = 375;  // Целевой код АЦП (~55°C)
    led_thermal_config.start_offset = 30;   // Начать упреждение за 30 кодов до целевой температуры

    // Коэффициенты масштабированы на 256 (например, Kp = 2.0 -> 2.0 * 256 = 512)
    led_thermal_config.kp           = 512;  // Kp = 2.0
    led_thermal_config.ki           = 64;   // Ki = 0.25
    led_thermal_config.k_predict    = 1024; // Kpredict = 4.0

    led_thermal_config.max_i_term   = 200;
    led_thermal_config.minimum_pwm  = 170;
    led_thermal_config.filter_shift = 3;    // Фильтр EMA температуры по 8 выборкам

    // гасим яркость быстро, возвращаем медленно 
    led_thermal_config.slew_release = 2;

    // Инициализируем структуру состояния регулятора
    pi_reg_init(&led_thermal_state, &led_thermal_config, raw_temperature_adc); //raw 25С = 271
}

void scan_limiters(void)
{
    #define MIN(a,b)			((a<b)?(a):(b))

    static uint8_t old_correction;

    if (driver_mode < MODE_ON)
        return; // зачем нам регулировать выключенный фонарь?

    //uint16_t calibr = (uint16_t)ee.usr.t_calibr + BASE_TEMPERATURE;

    //pic.correction = (uint8_t)ABS(pi_control(&pic, calibr - exact_temperature(temperature))); // вычисляем коррекцию.

    // Вычисляем необходимую величину ограничения мощности
    limiter = pi_reg_compute(&led_thermal_config, &led_thermal_state, raw_temperature_adc, step_user);

    if (old_correction != limiter)
    {
        uint8_t up_level = MAX_STEP - limiter;
        set_current_ll(MIN(up_level, step_user), SS_NO); // обновляем только когда включено для того,  чтобы не зажечь диод когда этого не надо.

/*         if ((ee.uif.ui_index > 1) && (mode_current == CMODE_TURBO)) // если UI > 1 значит включены дискретные UI, если режим Турбо
        {
            if (up_level < get_stepi(CMODE_MAX)) // Видим, что ТК увел яркость ниже максимального уровня и выключаем турбо.
            {
                mode_current = CMODE_MAX;
                ps.use_prev = false; // отключем возврат по двойному клику из Турбо. 12.10.2021
            }
        }
 */        old_correction = limiter;
    }
}

void reset_limiters(void)
{
    pi_reg_reset(&led_thermal_state, &led_thermal_config, raw_temperature_adc);
}