#ifndef PI_REG_H
#define PI_REG_H

#include <stdint.h>

// Масштабирование целочисленной арифметики (фиксированная точка)
#define PI_SCALE_SHIFT      8
#define PI_SCALE_FACTOR     (1 << PI_SCALE_SHIFT)

/**
 * @brief Структура конфигурации терморегулятора.
 * Все коэффициенты должны быть масштабированы на 256 (*256).
 */
typedef struct {
    uint16_t setpoint;         // Уставка температуры (9 бит, 0..511)
    uint16_t kp;               // Пропорциональный коэффициент (*256)
    uint16_t ki;               // Интегральный коэффициент (*256)
    uint16_t k_predict;        // Коэффициент упреждения по тренду нагрева (*256)
    uint16_t max_i_term;       // Предел интегратора сверху (Anti-windup)
    uint16_t minimum_pwm;      // Минимально допустимый ШИМ (защита драйвера LED)
    uint16_t start_offset;     // Смещение вниз от setpoint (в кодах АЦП) для начала регулирования
    uint8_t  filter_shift;     // Сдвиг EMA фильтра температуры (СТРОГО 1..8, оптимально 3 или 4)
    uint8_t  slew_attack;      // Скорость снижения яркости (рост Limiter за один шаг)
    uint8_t  slew_release;     // Скорость восстановления яркости (падение Limiter за один шаг)
    uint8_t emergency_threshold;// порог перехода в режим без slew rate 
} pi_reg_config_t;

/**
 * @brief Структура динамического состояния регулятора (RAM: 14 байт).
 */
typedef struct {
    uint32_t filtered_temp;    // Хранилище EMA фильтра температуры (10 бит << filter_shift)
    int32_t  heat_rate;        // Честный EMA фильтр скорости в fixed-point (*256)
    uint16_t prev_temp_10bit;  // Температура на прошлом шаге для расчета dT
    int32_t  integrator;       // Интегральная сумма ошибок (*256)
    uint16_t last_limiter;     // Значение ограничения на прошлом шаге
} pi_reg_state_t;

/**
 * @brief Сброс состояния регулятора при выключении системы.
 * @param state Указатель на структуру состояния.
 * @param config Указатель на структуру конфигурации (нужен для корректного заполнения фильтра).
 * @param current_adc_10bit Текущее сырое значение АЦП в момент выключения/сброса.
 */
void pi_reg_reset(pi_reg_state_t *state, const pi_reg_config_t *config, uint16_t current_adc_10bit);
// Макрос делает pi_reg_init синонимом pi_reg_reset. Компилятор просто подставит правильное имя.
#define pi_reg_init(state, config, initial_adc_10bit) pi_reg_reset(state, config, initial_adc_10bit)

void pi_reg_init(pi_reg_state_t *state, const pi_reg_config_t *config, uint16_t initial_adc_10bit);
uint16_t pi_reg_compute(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t raw_adc_10bit, uint16_t user_pwm);

#endif /* PI_REG_H */