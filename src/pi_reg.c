#include "pi_reg.h"
#include <string.h>

#define HR_SCALE_SHIFT            8

void pi_reg_reset(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t initial_adc_10bit) {
    if (!state) return;
    memset(state, 0, sizeof(*state));

    if (!config || config->filter_shift == 0 || config->filter_shift > 8) return;

    uint16_t safe_adc = (initial_adc_10bit > ADC_MAX_VALUE) ? ADC_MAX_VALUE : initial_adc_10bit;
    state->filtered_temp = (uint32_t)safe_adc << config->filter_shift;

    // Заполняем буфер истории начальным значением
    for (uint8_t i = 0; i < ADC_HISTORY_SIZE; i++) {
        state->raw_adc_history[i] = safe_adc;
    }
    state->history_index = 0;
    state->history_count = ADC_HISTORY_SIZE;
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

    // ========== 1. Фильтрация температуры (EMA) ==========
    state->filtered_temp = state->filtered_temp - (state->filtered_temp >> config->filter_shift) + raw_adc_10bit;
    uint16_t current_temp_10bit = (uint16_t)(state->filtered_temp >> config->filter_shift);
    state->current_temp_10bit = current_temp_10bit;

    // ========== 2. Расчет скорости нагрева за ~1 секунду ==========
    // Сохраняем фильтрованную температуру (единый сигнал с PI)
    state->raw_adc_history[state->history_index] = current_temp_10bit;
    state->history_index = (state->history_index + 1) & ADC_HISTORY_MASK;
    if (state->history_count < ADC_HISTORY_SIZE) state->history_count++;

    // Окно = 30 отсчётов (~1 секунда при 30Гц)
    uint8_t hist_window = 30;
    if (state->history_count < hist_window) hist_window = state->history_count;
    uint8_t old_index = (state->history_index - hist_window + ADC_HISTORY_SIZE) & ADC_HISTORY_MASK;
    int16_t raw_dT = (int16_t)current_temp_10bit - (int16_t)state->raw_adc_history[old_index];

    state->heat_rate = (int32_t)raw_dT;

    // ========== 3. Расчет ошибки ==========
    int32_t target_temp_10bit = (int32_t)config->setpoint << 1;
    int32_t start_temp_10bit = target_temp_10bit - ((int32_t)config->start_offset << 1);
    int32_t error = (int32_t)current_temp_10bit - start_temp_10bit;
    state->error = (int16_t)error;

    // Расстояние до уставки (в кодах АЦП, 10 бит)
    int32_t distance_to_target = target_temp_10bit - (int32_t)current_temp_10bit;
    if (distance_to_target < 0) distance_to_target = 0;

    int32_t p_term = 0;
    int32_t pi_term = 0;

    // P с гистерезисом ±1 код АЦП (фильтр шума)
    int32_t filtered_error = error;
    int32_t err_diff = error - state->prev_error;
    if (err_diff < 0) err_diff = -err_diff;
    if (err_diff <= 1) {
        filtered_error = state->prev_error;  // Шум — сохраняем предыдущее
    }
    state->prev_error = (int16_t)error;

    if (filtered_error > 0) {
        int32_t clipped_error = (filtered_error > (int32_t)ADC_MAX_VALUE) ? (int32_t)ADC_MAX_VALUE : filtered_error;
        p_term = clipped_error * (int32_t)config->kp;
    }

    // Интегратор: работает всегда при error > 0, скорость зависит от HR
    if (error > 0) {
        int32_t clipped_error = (error > (int32_t)ADC_MAX_VALUE) ? (int32_t)ADC_MAX_VALUE : error;
        int32_t delta_i = clipped_error * (int32_t)config->ki;

        // Скорость интегрирования по HR
        if (state->heat_rate > 6) {
            delta_i <<= 2;  // Быстрый нагрев → ×4
        } else if (state->heat_rate > 3) {
            delta_i <<= 1;  // Средний нагрев → ×2
        } else if (state->heat_rate <= 0) {
            delta_i >>= 2;  // HR≤0 (стационар/остывание) → медленно (÷4)
        }
        // HR 1..3 → ×1 (норма)

        // Торможение около уставки
        if (distance_to_target < (4 << 1)) {
            delta_i >>= 3;
        } else if (distance_to_target < (8 << 1)) {
            delta_i >>= 2;
        } else if (distance_to_target < (15 << 1)) {
            delta_i >>= 1;
        }

        // Anti-Windup: не накапливаем если близко к потолку
        if (max_allowed_limiter > 0 && state->current_limiter >= max_allowed_limiter - 2) {
            // Не обновляем — уже насыщаемся
        } else {
            int32_t new_integrator = state->integrator + delta_i;
            int32_t max_i_scaled = (int32_t)config->max_i_term << PI_SCALE_SHIFT;
            if (new_integrator > max_i_scaled) new_integrator = max_i_scaled;
            if (new_integrator < 0) new_integrator = 0;
            state->integrator = new_integrator;
        }
    }
    else {
        // error <= 0 — стравливаем интегратор (всегда, независимо от HR)
        // Скорость стравливания пропорциональна величине ошибки
        int32_t bleed = state->integrator >> 9;
        if (bleed == 0) bleed = 1;
        state->integrator -= bleed;
        if (state->integrator < 0) state->integrator = 0;
    }

    pi_term = p_term + state->integrator;

    // ========== 4. Feedforward упреждение ==========
    int32_t predict_term = 0;

    if (state->heat_rate > 0 && config->k_predict > 0) {
        int32_t predict_dist = (int32_t)config->predict_distance << 1;

        if (distance_to_target <= 0) {
            // За уставкой — полная компенсация
            // predict = heat_rate * k_predict (k_predict уже в единицах ШИМ/(код/сек))
            predict_term = state->heat_rate * (int32_t)config->k_predict;
        }
        else if (predict_dist > 0 && distance_to_target < predict_dist) {
            // В зоне predict_distance — плавное нарастание
            int32_t proximity = predict_dist - distance_to_target;
            int32_t base_predict = state->heat_rate * (int32_t)config->k_predict;
            predict_term = (base_predict * proximity) / predict_dist;
        }
    }

    // Отладочные значения в масштабе ШИМ
    state->p_term_out = (uint16_t)(p_term >> PI_SCALE_SHIFT);
    state->i_term_out = (uint16_t)(state->integrator >> PI_SCALE_SHIFT);
    state->predict_term_out = (uint16_t)(predict_term >> PI_SCALE_SHIFT);

    // ========== 5. Формирование лимитера ==========
    int32_t target_limiter = (pi_term + predict_term) >> PI_SCALE_SHIFT;

    if (target_limiter > max_allowed_limiter) target_limiter = max_allowed_limiter;
    if (target_limiter < 0)                   target_limiter = 0;

    // ========== 6. Slew rate limiter (симметричный) ==========
    int32_t working_limiter = (int32_t)state->current_limiter;

    if (target_limiter > working_limiter) {
        int32_t diff = target_limiter - working_limiter;
        if (diff > (int32_t)config->slew_rise) {
            working_limiter += (int32_t)config->slew_rise;
        } else {
            working_limiter = target_limiter;
        }
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
