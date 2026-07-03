#include "pi_reg.h"
#include <string.h>


void pi_reg_reset(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t initial_adc_10bit) {
    if (!state) {
        return;
    }

    // Использование sizeof(*state) защищает от ошибок при изменении имени или типа структуры
    memset(state, 0, sizeof(*state));

    if (!config || config->filter_shift == 0 || config->filter_shift > 8) {
        return;
    }

    // Жесткое ограничение входного значения, если передали некорректные данные АЦП
    uint16_t safe_adc = (initial_adc_10bit > ADC_MAX_VALUE) ? ADC_MAX_VALUE : initial_adc_10bit;

    state->filtered_temp = (uint32_t)safe_adc << config->filter_shift;
    state->prev_temp_10bit = safe_adc;
}

uint16_t pi_reg_compute(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t raw_adc_10bit, uint16_t user_pwm, pi_reg_debug_t *dbg) {
    // 0. Входной контроль конфигурации
    if (!state || !config || config->filter_shift == 0 || config->filter_shift > 8 ||
        config->kp > MAX_ALLOWED_KP || config->ki > MAX_ALLOWED_KI || 
        config->k_predict > MAX_ALLOWED_KPREDICT || config->max_i_term > MAX_ALLOWED_MAX_I) {
        return state ? state->current_limiter : 0;
    }

    // Защита от передачи 12-битного АЦП или случайных выбросов выше крышки шкалы
    if (raw_adc_10bit > ADC_MAX_VALUE) {
        raw_adc_10bit = ADC_MAX_VALUE;
    }

    // Вычисляем динамический потолок для limiter
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

    // 3. Расчет статической ошибки ПИ-компоненты
    int32_t target_temp_10bit = (int32_t)config->setpoint << 1;
    int32_t start_temp_10bit = target_temp_10bit - ((int32_t)config->start_offset << 1);
    int32_t error = (int32_t)current_temp_10bit - start_temp_10bit;

    int32_t pi_term = 0;

    if (error > 0) {
        int32_t clipped_error = (error > (int32_t)ADC_MAX_VALUE) ? (int32_t)ADC_MAX_VALUE : error;
        int32_t p_term = clipped_error * (int32_t)config->kp;

        int32_t delta_i = clipped_error * (int32_t)config->ki;
        int32_t new_integrator = state->integrator + delta_i;

        int32_t max_i_scaled = (int32_t)config->max_i_term << PI_SCALE_SHIFT;
        if (new_integrator > max_i_scaled) {
            new_integrator = max_i_scaled;
        }

        // Улучшенный Anti-Windup: предотвращает ложное отсекание при нулевом или минимальном ШИМ
        if (max_allowed_limiter > 0 && state->current_limiter >= max_allowed_limiter) {
            // Регулятор уже выжал максимум из доступного диапазона — фиксируем И-составляющую
        } else {
            state->integrator = new_integrator;
        }

        pi_term = p_term + state->integrator;
    } else {
        // Мягкое затухание интегратора вне зоны перегрева
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

    // 5. Формирование целевого лимитера
    int32_t target_limiter = (pi_term + predict_term) >> PI_SCALE_SHIFT;

    if (target_limiter > max_allowed_limiter) target_limiter = max_allowed_limiter;
    if (target_limiter < 0)                   target_limiter = 0;

/* ================== ВСТАВКА ДЛЯ ОТЛАДКИ НАЧАЛО ================== */
    if (dbg) {
        dbg->temp = current_temp_10bit;
        dbg->error = error;
        dbg->heat_rate = state->heat_rate;
        // Пересчитываем чистый P_term для отладки (так как в pi_term уже сидит сумма P + I)
        dbg->p_term = (error > 0) ? (error * (int32_t)config->kp) : 0;
        dbg->i_term = state->integrator;
        dbg->predict = predict_term;
        dbg->limiter = (uint16_t)target_limiter; // Записываем то, что ХОЧЕТ регулятор
    }
    /* =================== ВСТАВКА ДЛЯ ОТЛАДКИ КОНЕЦ =================== */

    // 6. Асимметричная скорость реакции (Мгновенный сброс / плавный возврат яркости)
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