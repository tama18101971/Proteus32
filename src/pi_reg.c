#include "pi_reg.h"
#include <string.h>

#define MAX_ADC_DELTA_PER_CYCLE   8
#define HEAT_RATE_FILTER_SHIFT    3
#define HR_SCALE_SHIFT            8

void pi_reg_reset(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t initial_adc_10bit) {
    if (!state) return;
    memset(state, 0, sizeof(*state));

    if (!config || config->filter_shift == 0 || config->filter_shift > 8) return;

    uint16_t safe_adc = (initial_adc_10bit > ADC_MAX_VALUE) ? ADC_MAX_VALUE : initial_adc_10bit;
    state->filtered_temp = (uint32_t)safe_adc << config->filter_shift;
    state->prev_temp_10bit = safe_adc;
}

uint16_t pi_reg_compute(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t raw_adc_10bit, uint16_t user_pwm) {
    if (!state || !config || config->filter_shift == 0 || config->filter_shift > 8 ||
        config->kp > MAX_ALLOWED_KP || config->ki > MAX_ALLOWED_KI || 
        config->k_predict > MAX_ALLOWED_KPREDICT || config->max_i_term > MAX_ALLOWED_MAX_I) {
        return state ? state->current_limiter : 0;
    }

    if (raw_adc_10bit > ADC_MAX_VALUE) raw_adc_10bit = ADC_MAX_VALUE;

    int32_t max_allowed_limiter = 0;
    if ((int32_t)user_pwm > (int32_t)config->minimum_pwm) {
        max_allowed_limiter = (int32_t)user_pwm - (int32_t)config->minimum_pwm;
    }

    // 1. Фильтрация температуры (EMA)
    state->filtered_temp = state->filtered_temp - (state->filtered_temp >> config->filter_shift) + raw_adc_10bit;
    uint16_t current_temp_10bit = (uint16_t)(state->filtered_temp >> config->filter_shift);

    // 2. Расчет тренда скорости изменения температуры
    int16_t raw_dT = (int16_t)current_temp_10bit - (int16_t)state->prev_temp_10bit;
    state->prev_temp_10bit = current_temp_10bit;

    if (raw_dT > MAX_ADC_DELTA_PER_CYCLE)  raw_dT = MAX_ADC_DELTA_PER_CYCLE;
    if (raw_dT < -MAX_ADC_DELTA_PER_CYCLE) raw_dT = -MAX_ADC_DELTA_PER_CYCLE;

    int32_t raw_dT_fp = (int32_t)raw_dT << HR_SCALE_SHIFT;
    state->heat_rate += (raw_dT_fp - state->heat_rate) >> HEAT_RATE_FILTER_SHIFT;

    // 3. Расчет статической ошибки и адаптивное управление интегратором
    int32_t target_temp_10bit = (int32_t)config->setpoint << 1;
    int32_t start_temp_10bit = target_temp_10bit - ((int32_t)config->start_offset << 1);
    int32_t error = (int32_t)current_temp_10bit - start_temp_10bit;
    
    // Сохраняем ошибку для отладки
    state->error = (int16_t)error;

    int32_t p_term = 0;
    int32_t pi_term = 0;

    if (error > 0) {
        int32_t clipped_error = (error > (int32_t)ADC_MAX_VALUE) ? (int32_t)ADC_MAX_VALUE : error;
        p_term = clipped_error * (int32_t)config->kp;

        int32_t delta_i = clipped_error * (int32_t)config->ki;
        int32_t new_integrator = state->integrator;

        // Если температура растет — копим ошибку
        if (state->heat_rate >= 0) {
            new_integrator += delta_i;
        } 
        // Если температура падает — сдуваем интегратор пропорционально скорости остывания
        else {
            int32_t melt = ((-state->heat_rate) * (int32_t)config->ki) >> 4;
            if (melt == 0) melt = 4;
            new_integrator -= melt;
        }

        int32_t max_i_scaled = (int32_t)config->max_i_term << PI_SCALE_SHIFT;
        if (new_integrator > max_i_scaled) new_integrator = max_i_scaled;
        if (new_integrator < 0) new_integrator = 0;

        if (max_allowed_limiter > 0 && state->current_limiter >= max_allowed_limiter) {
            // Фиксация Anti-Windup
        } else {
            state->integrator = new_integrator;
        }

        pi_term = p_term + state->integrator;
    } else {
        if (state->integrator > 0) {
            int32_t bleed = state->integrator >> 5;
            if (bleed == 0) bleed = 1;
            state->integrator -= bleed;
            if (state->integrator < 0) state->integrator = 0;
        }
        pi_term = state->integrator;
    }

    // 4. Расчет Feedforward упреждения
    int32_t predict_term = 0;
    int32_t start_offset_10bit = (int32_t)config->start_offset << 1;

    if (state->heat_rate > 0) {
        int32_t distance = target_temp_10bit - (int32_t)current_temp_10bit;
        int32_t base_predict = (state->heat_rate * (int32_t)config->k_predict) >> HR_SCALE_SHIFT;

        if (distance <= 0) {
            predict_term = base_predict;
        } 
        else if (start_offset_10bit > 0 && distance < start_offset_10bit) {
            int32_t proximity = start_offset_10bit - distance;
            predict_term = (base_predict * proximity) / start_offset_10bit;
        }
    }

    // Запись отладочных значений в масштабе реального ШИМ (сдвиг >> 8)
    state->p_term_out = (uint16_t)(p_term >> PI_SCALE_SHIFT);
    state->i_term_out = (uint16_t)(state->integrator >> PI_SCALE_SHIFT);
    state->predict_term_out = (uint16_t)(predict_term >> PI_SCALE_SHIFT);

    // 5. Формирование целевого лимитера
    int32_t target_limiter = (pi_term + predict_term) >> PI_SCALE_SHIFT;

    if (target_limiter > max_allowed_limiter) target_limiter = max_allowed_limiter;
    if (target_limiter < 0)                   target_limiter = 0;

    // 6. Асимметричный фильтр скорости изменения выхода
    int32_t working_limiter = (int32_t)state->current_limiter;

    if (target_limiter > working_limiter) {
        working_limiter = target_limiter;
    } else {
        int32_t diff = working_limiter - target_limiter;
        if (diff > (int32_t)config->slew_release) {
            working_limiter -= (int32_t)config->slew_release;
        } else {
            working_limiter = target_limiter;
        }
    }

    state->current_limiter = (uint16_t)working_limiter;
    return state->current_limiter;
}