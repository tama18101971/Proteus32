#include "pi_reg.h"

// Ограничение максимальной скорости изменения температуры за 1 шаг таймера (защита от шума АЦП)
#define MAX_RAW_DT_THRESHOLD   8

// Внутренний сдвиг для fixed-point представления heat_rate (масштаб *256)
#define HR_SCALE_SHIFT         8

// Жесткие пределы безопасных значений для защиты от знакового переполнения int32_t при умножении
#define MAX_ALLOWED_KP         4096  // Максимальный Kp = 16.0
#define MAX_ALLOWED_KI         1024  // Максимальный Ki = 4.0
#define MAX_ALLOWED_KPREDICT   8192  // Максимальный Kpredict = 32.0

void pi_reg_init(pi_reg_state_t *state, const pi_reg_config_t *config, uint16_t initial_adc_10bit) {
    if (!state || !config || config->filter_shift == 0 || config->filter_shift > 8) {
        return;
    }
    state->filtered_temp = (uint32_t)initial_adc_10bit << config->filter_shift;
    state->prev_temp_10bit = initial_adc_10bit;
    state->heat_rate = 0;
    state->integrator = 0;
    state->last_limiter = 0;
}

/**
 * @brief Основной вычислительный цикл (вызывается каждые 20-50 мс).
 * Полностью оптимизирован, защищен от переполнений и снабжен аварийным обходом Slew Rate.
 */
uint16_t pi_reg_compute(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t raw_adc_10bit, uint16_t user_pwm) {
    // 0. Входной контроль (Параметры и Сдвиг). Защищает библиотеку от некорректных конфигов.
    if (!state || !config || config->filter_shift == 0 || config->filter_shift > 8 ||
        config->kp > MAX_ALLOWED_KP || config->ki > MAX_ALLOWED_KI || config->k_predict > MAX_ALLOWED_KPREDICT) {
        return state ? state->last_limiter : 0;
    }

    // Вычисляем максимально возможное ограничение с учетом минимальной мощности драйвера
    int32_t max_allowed_limiter = 0;
    if ((int32_t)user_pwm > (int32_t)config->minimum_pwm) {
        max_allowed_limiter = (int32_t)user_pwm - (int32_t)config->minimum_pwm;
    }

    // 1. Фильтрация текущей температуры (EMA)
    state->filtered_temp = state->filtered_temp - (state->filtered_temp >> config->filter_shift) + raw_adc_10bit;
    uint16_t current_temp_10bit = (uint16_t)(state->filtered_temp >> config->filter_shift);

    // 2. Расчет тренда скорости (Честный fixed-point EMA)
    int16_t raw_dT = (int16_t)current_temp_10bit - (int16_t)state->prev_temp_10bit;
    state->prev_temp_10bit = current_temp_10bit;

    if (raw_dT > MAX_RAW_DT_THRESHOLD)  raw_dT = MAX_RAW_DT_THRESHOLD;
    if (raw_dT < -MAX_RAW_DT_THRESHOLD) raw_dT = -MAX_RAW_DT_THRESHOLD;

    int32_t raw_dT_fp = (int32_t)raw_dT << HR_SCALE_SHIFT;
    state->heat_rate += (raw_dT_fp - state->heat_rate) >> 3;

    // 3. Вычисление ошибки статического ПИ-регулятора
    int32_t target_temp_10bit = (int32_t)config->setpoint << 1;
    int32_t start_temp_10bit = target_temp_10bit - ((int32_t)config->start_offset << 1);
    int32_t error = (int32_t)current_temp_10bit - start_temp_10bit;

    int32_t pi_term = 0;

    if (error > 0) {
        // Ограничение ошибки для 10-битного АЦП. Гарантирует абсолютную безопасность p_term.
        int32_t clipped_error = (error > 1023) ? 1023 : error;

        int32_t p_term = clipped_error * (int32_t)config->kp;

        // Расчет delta_i и сложение математически прозрачны и 100% безопасны
        int32_t delta_i = clipped_error * (int32_t)config->ki;
        int32_t new_integrator = state->integrator + delta_i;

        // Насыщение интегратора по заданному пользователем потолку
        int32_t max_i_scaled = (int32_t)config->max_i_term << PI_SCALE_SHIFT;
        if (new_integrator > max_i_scaled) {
            new_integrator = max_i_scaled;
        }

        // Оцениваем, какой стала бы PI-составляющая ограничения с новым потенциальным интегратором
        int32_t estimated_pi_limiter = (p_term + new_integrator) >> PI_SCALE_SHIFT;

        // Если мы не уперлись в физический потолок мощности, разрешаем обновление интегратора (Anti-Windup)
        if (estimated_pi_limiter < max_allowed_limiter) {
            state->integrator = new_integrator;
        }

        pi_term = p_term + state->integrator;
    } else {
        // Математически красивый Bleeding без мертвых зон
        if (state->integrator > 0) {
            int32_t bleed = state->integrator >> 5;
            if (bleed == 0) {
                bleed = 1;
            }
            state->integrator -= bleed;
            if (state->integrator < 0) {
                state->integrator = 0;
            }
        }
        pi_term = state->integrator;
    }

    // 4. Расчет динамического упреждения Feedforward с защитой от деления на ноль
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

    // 5. Итоговое целевое ограничение мощности
    int32_t target_limiter = (pi_term + predict_term) >> PI_SCALE_SHIFT;

    // Ограничение диапазонов target_limiter относительно физического насыщения
    if (target_limiter > max_allowed_limiter) {
        target_limiter = max_allowed_limiter;
    }
    if (target_limiter < 0) {
        target_limiter = 0;
    }

    // 6. Асимметричный Slew Rate Limiter с аварийным обходом (Emergency Bypass)
    int32_t current_limiter = (int32_t)state->last_limiter;

    // Проверяем критический перегрев: если температура сильно выше уставки,
    // отключаем плавность Slew Rate ради спасения кристалла светодиода
    if (error > (int32_t)config->emergency_threshold) {
        current_limiter = target_limiter;
    }
    else if (error <= 0 && predict_term == 0) {
        // Режим мягкого остывания (штатный)
        if (current_limiter > (int32_t)config->slew_release) {
            current_limiter -= (int32_t)config->slew_release;
        } else {
            current_limiter = 0;
        }
    } else {
        // Штатный режим изменения мощности (работает ограничение скорости 5-10 секунд)
        int32_t limiter_diff = target_limiter - current_limiter;

        if (limiter_diff > 0) {
            // Мощность нужно увеличить (target > current) -> slew_attack
            if (limiter_diff > (int32_t)config->slew_attack) {
                current_limiter += (int32_t)config->slew_attack;
            } else {
                current_limiter = target_limiter;
            }
        } else if (limiter_diff < 0) {
            // Мощность возвращается (Limiter падает) -> slew_release
            if (limiter_diff < -(int32_t)config->slew_release) {
                current_limiter -= (int32_t)config->slew_release;
            } else {
                current_limiter = target_limiter;
            }
        }
    }

    state->last_limiter = (uint16_t)current_limiter;
    return state->last_limiter;
}

