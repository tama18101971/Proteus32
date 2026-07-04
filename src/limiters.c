#include "adc.h"
#include "ui.h"
#include "limiters.h"
#include "wl_flash.h"
#include "config.h"

/* ============ СТРУКТУРЫ РЕГУЛЯТОРА ============ */

ThermalLimiterState  state;
ThermalLimiterConfig config = {
    .temp_setpoint = 700,    /* Заданная температура (код АЦП) */
    .Kp            = 64,     /* Коэффициент P (реальное 0.25) */
    .Ki            = 2,      /* Коэффициент I (реальное 0.008) */
    .I_max         = 26000,  /* Ограничение интегратора */
    .rate_warm     = 3,      /* Скорость гашения (быстро) */
    .rate_cool     = 2,      /* Скорость восстановления (медленно) */
    .max_limit     = 511,    /* Макс. limiter (9 бит) */
    .filter_shift  = 3,      /* IIR: tau = 8 выборок = 0.26 сек */
    .ntc_upper_leg = 1       /* NTC в верхнем плече делителя */
};

uint16_t limiter = 0;

void sys_init(void)
{
    /* Инициализация регулятора (обнуление состояния) */
    ThermalLimiter_Init(&state, &config);
}

void scan_limiters(void)
{
#define MIN(a, b) ((a < b) ? (a) : (b))

    static uint8_t old_correction;

    if (driver_mode < MODE_ON)
        return;

    /* Шаг 2: Вызвать регулятор → получить limiter
        * limiter = 0..user_pwm
        * pwm_out = user_pwm - limiter (пользователь управляет ШИМ) */
    limiter = ThermalLimiter_Update(&state, &config, raw_temperature_adc, step_user);

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
    ThermalLimiter_Reset(&state);
    limiter = 0;
}