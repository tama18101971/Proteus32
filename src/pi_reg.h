#ifndef PI_REG_H
#define PI_REG_H

#include <stdint.h>

#define ADC_BITS                10
#define ADC_MAX_VALUE           ((1U << ADC_BITS) - 1)

#define MAX_ALLOWED_KP          4096   // Максимальный Kp = 16.0
#define MAX_ALLOWED_KI          1024   // Максимальный Ki = 4.0
#define MAX_ALLOWED_KPREDICT    8192   // Максимальный Kpredict = 32.0
#define MAX_ALLOWED_MAX_I       65535  // Полностью безопасный максимум интегратора

// Масштабирование целочисленной арифметики (фиксированная точка)
#define PI_SCALE_SHIFT          8
#define PI_SCALE_FACTOR         (1 << PI_SCALE_SHIFT)

// Размер буфера истории АЦП для расчета скорости нагрева
// 32 отсчёта при 50 Гц ≈ 640 мс
#define ADC_HISTORY_SIZE        32
#define ADC_HISTORY_MASK        (ADC_HISTORY_SIZE - 1)

/**
 * @brief Структура конфигурации терморегулятора.
 */
typedef struct {
    uint16_t setpoint;          // Уставка температуры (9 бит, 0..511). Для перевода в 10-бит АЦП умножается на 2.
    uint16_t kp;                // Пропорциональный коэффициент (*256)
    uint16_t ki;                // Интегральный коэффициент (*256)
    uint16_t k_predict;         // Коэффициент упреждения по тренду нагрева (*256)
    uint16_t max_i_term;        // Предел интегратора сверху в единицах ШИМ (Anti-windup)
    uint16_t minimum_pwm;       // Минимально допустимый ШИМ (защита драйвера LED)
    uint16_t start_offset;      // Смещение вниз от setpoint (в кодах АЦП) для начала регулирования
    uint16_t predict_distance;  // Дистанция начала работы FeedForward (в кодах АЦП)
    uint8_t  filter_shift;      // Сдвиг EMA фильтра температуры (СТРОГО 1..8)
    uint8_t  slew_rise;         // Скорость нарастания limiter (максимальный рост за шаг)
    uint8_t  slew_release;      // Скорость падения limiter (максимальное падение за шаг)
} pi_reg_config_t;

/**
 * @brief Структура динамического состояния регулятора (с полями отладки).
 */
typedef struct {
    uint32_t filtered_temp;     // Хранилище EMA фильтра температуры (10 бит << filter_shift)
    int32_t  heat_rate;         // Скорость нагрева за последние ~320мс (*256)
    int32_t  integrator;        // Интегральная сумма ошибок (*256)
    uint16_t current_limiter;   // Текущее значение ограничения

    // Circular buffer истории АЦП для расчета heat_rate
    uint16_t raw_adc_history[ADC_HISTORY_SIZE];
    uint8_t  history_index;     // Текущий индекс в буфере
    uint8_t  history_count;     // Количество заполненных элементов

    // --- ПОЛЯ ОТЛАДКИ ---
    uint16_t current_temp_10bit; // Текущая отфильтрованная температура (10 бит)
    int16_t  error;             // Текущая статическая ошибка (коды АЦП)
    int16_t  prev_error;        // Предыдущая ошибка (для гистерезиса P)
    uint16_t p_term_out;        // Пропорциональная часть в масштабе ШИМ
    uint16_t i_term_out;        // Интегральная часть в масштабе ШИМ
    uint16_t predict_term_out;  // Компонента упреждения в масштабе ШИМ
} pi_reg_state_t;

void pi_reg_reset(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t initial_adc_10bit);

#define pi_reg_init(config, state, initial_adc_10bit) pi_reg_reset(config, state, initial_adc_10bit)

uint16_t pi_reg_compute(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t raw_adc_10bit, uint16_t user_pwm);

#endif /* PI_REG_H */