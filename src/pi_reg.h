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

// Максимальное изменение сырого АЦП за один цикл вызова (защита от импульсных помех)
#define MAX_ADC_DELTA_PER_CYCLE   8

// Коэффициент фильтрации тренда скорости нагрева (EMA shift)
#define HEAT_RATE_FILTER_SHIFT    3

// Внутренний сдвиг для fixed-point представления heat_rate (масштаб *256)
#define HR_SCALE_SHIFT            8


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
    uint8_t  filter_shift;      // Сдвиг EMA фильтра температуры (СТРОГО 1..8)
    uint8_t  slew_release;      // Скорость восстановления яркости (падение limiter за один шаг)
} pi_reg_config_t;

/**
 * @brief Структура динамического состояния регулятора (RAM: 16 байт).
 */
typedef struct {
    uint32_t filtered_temp;     // Хранилище EMA фильтра температуры (10 бит << filter_shift)
    int32_t  heat_rate;         // Честный EMA фильтр скорости в fixed-point (*256)
    uint16_t prev_temp_10bit;   // Температура на прошлом шаге для расчета dT
    int32_t  integrator;        // Интегральная сумма ошибок (*256)
    uint16_t current_limiter;   // Текущее значение ограничения
} pi_reg_state_t;

/**
 * @brief Структура для съема внутренних параметров регулятора (только для отладки).
 */
typedef struct {
    uint16_t temp;          // Текущая отфильтрованная температура (10 бит)
    int32_t  error;         // Текущая статическая ошибка
    int32_t  heat_rate;     // Отфильтрованный тренд скорости (*256)
    int32_t  p_term;        // Пропорциональная составляющая (до сдвига SCALE)
    int32_t  i_term;        // Интегральная составляющая (до сдвига SCALE)
    int32_t  predict;       // Компонента упреждения Feedforward (до сдвига SCALE)
    uint16_t limiter;       // Итоговое ограничение ШИМ
} pi_reg_debug_t;


void pi_reg_reset(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t initial_adc_10bit);

/**
 * @brief Макрос обратной совместимости. Использование sizeof(*state) гарантирует безопасность.
 */
#define pi_reg_init(config, state, initial_adc_10bit) pi_reg_reset(config, state, initial_adc_10bit)

uint16_t pi_reg_compute(const pi_reg_config_t *config, pi_reg_state_t *state, uint16_t raw_adc_10bit, uint16_t user_pwm, pi_reg_debug_t *dbg);

#endif /* PI_REG_H */