#include "pid.h"

/*
 * Реализация выбрана максимально простой и быстрой:
 *  - все коэффициенты в Q8 (умножение/сдвиг вместо float)
 *  - производная берётся по input, а не по error -> нет "derivative kick"
 *    при скачке setpoint
 *  - anti-windup: интеграл ограничен диапазоном выхода *PID_SCALE,
 *    плюс clamping (если выход уже в насыщении и ошибка того же знака —
 *    интеграл не растёт дальше)
 *
 * Масштаб входа (10 бит) и выхода (12 бит) разные, поэтому коэффициенты
 * Kp/Ki/Kd по сути пересчитывают "единицы входа" в "единицы выхода".
 * Если нужно по-другому — масштабируйте setpoint/input заранее.
 */

void PID_Init(PID_t *pid, int32_t kp, int32_t ki, int32_t kd, int32_t setpoint)
{
    pid->Kp = kp;
    pid->Ki = ki;
    pid->Kd = kd;
    pid->setpoint = setpoint;
    pid->integral = 0;
    pid->prev_input = setpoint;
    pid->out_min = 0;
    pid->out_max = PID_OUT_MAX;
}

void PID_Reset(PID_t *pid, int32_t current_input)
{
    pid->integral = 0;
    pid->prev_input = current_input;
}

int32_t PID_Compute(PID_t *pid, int32_t input)
{
    int32_t error = pid->setpoint - input;

    /* --- P --- */
    int32_t p_term = pid->Kp * error;

    /* --- I --- (накопление уже умноженного на Ki значения) */
    pid->integral += pid->Ki * error;

    /* anti-windup: ограничиваем интегральную составляющую диапазоном выхода */
    int32_t i_limit = (int32_t)(pid->out_max - pid->out_min) << PID_SHIFT;
    if (pid->integral > i_limit)  pid->integral = i_limit;
    if (pid->integral < -i_limit) pid->integral = -i_limit;

    /* --- D --- (по входу, не по ошибке — гасит derivative kick) */
    int32_t d_term = pid->Kd * (pid->prev_input - input);
    pid->prev_input = input;

    /* --- сумма, перевод из Q8 в целые единицы выхода --- */
    int32_t output = (p_term + pid->integral + d_term) >> PID_SHIFT;

    /* финальное ограничение выхода */
    if (output > pid->out_max) {
        output = pid->out_max;
    } else if (output < pid->out_min) {
        output = pid->out_min;
    }

    return output;
}
