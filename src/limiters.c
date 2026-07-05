#include "adc.h"
#include "ui.h"
#include "limiters.h"
#include "wl_flash.h"
#include "config.h"

/* ============ СТРУКТУРЫ РЕГУЛЯТОРА ============ */

ThermalLimiterState_t  state;
const ThermalLimiterConfig_t config = {
    .kp_left_shift        = 5,   /* Zone 3 P: error << 5 — прежнее значение 4 давало
                                     установившийся перелёт ~+17 кодов (P=272 при error=17);
                                     при том же P error нужен вдвое меньше (~8-9) */
    .ki_decimation_shift  = 7,   /* Заряд I раз в 128 тиков (~4.3 сек) */
    .ki_left_shift        = 0,   /* Zones 2/3 I: error << 0 (плавный заряд) */
    .t_anticipate_offset  = 30,  /* Зона anticipation: [670, 700) — компромисс между
                                    20 (слишком узко, полный сброс I каждый цикл) и
                                    50 (слишком широко, I рос при остывании) */
    .filter_shift         = 1,   /* EMA:  */
    .max_drop_rate        = 8,   /* Скорость гашения (240/сек) */
    .max_recovery_rate    = 3,   /* Скорость восстановления (90/сек) */
    .overheat_leak_decimation_shift = 6  /* Утечка Zone2-latch раз в X тик */

};

uint16_t limiter = 0;

void sys_init(void)
{
    /* Инициализация регулятора (обнуление состояния) */
    ThermalLimiter_Init(&state);
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
    limiter = ThermalLimiter_Update(&config, &state, raw_temperature_adc, T_SET, step_user);

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
    /* Инициализация регулятора (обнуление состояния) */
    ThermalLimiter_Init(&state);
    limiter = 0;
}