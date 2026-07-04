/*
 * thermal_limiter.c
 *
 * Реализация терморегулятора мощности светодиода
 *
 * Структура алгоритма (вызывается каждые 33 мс / 30 Гц):
 *
 *   [АЦП raw] --> [IIR фильтр] --> [ошибка] --> [P + I] --> [slew rate] --> [limiter]
 *                                         ^                      |
 *                                         |                      v
 *                                   [setpoint]           [user_pwm - limiter = PWM]
 *
 * Свойства:
 *   - Только целочисленная арифметика (нет float)
 *   - Аппаратно-независимый (работает на любом МК)
 *   - Асимметричное ограничение скорости (нагрев быстрее охлаждения)
 *   - Защита от насыщения интегратора (anti-windup)
 *   - Фильтрация шума АЦП (IIR)
 */

#include "thermal_limiter.h"

/*
 * tl_div_round — деление целых чисел с округлением
 *
 * Вычисляет (num / den) с округлением к ближайшему.
 * Корректно работает с отрицательными числами (округление к нулю).
 *
 * Пример:
 *   tl_div_round(100, 3) = 33   (100/3 = 33.33 → 33)
 *   tl_div_round(-100, 3) = -33 (-100/3 = -33.33 → -33)
 *   tl_div_round(10, 8) = 1     (10/8 = 1.25 → 1)
 *
 * Параметры:
 *   num — числитель (знаковое, 32 бита)
 *   den — знаменатель (беззнаковый, 16 бит, должен быть > 0)
 *
 * Возвращает:
 *   Результат деления с округлением (знаковый, 32 бита)
 */
static int32_t tl_div_round(int32_t num, uint16_t den)
{
    if (num >= 0)
        return (num + (int32_t)(den / 2)) / (int32_t)den;
    return -(((-num) + (int32_t)(den / 2)) / (int32_t)den);
}

/*
 * ThermalLimiter_Init — инициализация состояния регулятора
 *
 * Обнуляет:
 *   - integral: интегральный аккумулятор (ошибки не накоплены)
 *   - filtered_temp: фильтр температуры (значение 0 → при первом вызове
 *     фильтр инициализируется текущим АЦП без задержки)
 *   - prev_limiter: ограничитель (0 = полная яркость)
 *
 * Параметры:
 *   s   — структура состояния (RAM)
 *   cfg — конфигурация (не используется, но передается для единообразия)
 */
void ThermalLimiter_Init(ThermalLimiterState *s, const ThermalLimiterConfig *cfg)
{
    s->integral = 0;
    s->filtered_temp = 0;
    s->prev_filtered_temp = 0;
    s->prev_limiter = 0;
    (void)cfg;
}

/*
 * ThermalLimiter_Reset — полный сброс состояния
 *
 * Используется при:
 *   - Горячем перезапуске (смена уставки без перезагрузки)
 *   - Выходе из режима экономии (standby)
 *   - Обнаружении аварийной ситуации
 *
 * Эквивалентен вызову ThermalLimiter_Init().
 */
void ThermalLimiter_Reset(ThermalLimiterState *s)
{
    s->integral = 0;
    s->filtered_temp = 0;
    s->prev_filtered_temp = 0;
    s->prev_limiter = 0;
}

/*
 * ThermalLimiter_Update — основной алгоритм регулирования
 *
 * Вызывается каждые 33 мс (30 Гц) из периодического таймера или
 * из основного цикла с задержкой Delay_Ms(33).
 *
 * Алгоритм пошагово:
 *
 *   ШАГ 1: IIR-фильтрация температуры
 *     Формула: filtered += (raw - filtered) >> shift
 *     При shift=5:tau = 32 выборки = 1.06 секунды
 *     Сглаживает шум АЦП, но добавляет задержку.
 *     При первом вызове: filtered = raw (мгновенная инициализация)
 *
 *   ШАГ 2: Вычисление ошибки
 *     error = filtered - setpoint (при NTC в верхнем плече)
 *     error > 0 → перегрев (нужно уменьшить PWM)
 *     error < 0 → недогрев (нужно увеличить PWM)
 *
 *   ШАГ 3: Пропорциональная составляющая P
 *     P = (|error| * Kp) >> 8 * sign(error)
 *     Реагирует на текущую ошибку. Быстро, но не устраняет
 *     постоянную ошибку (steady-state error).
 *
 *   ШАГ 4: Интегральная составляющая I (с anti-windup)
 *     integral += error
 *     I = (|integral| * Ki) >> 8 * sign(integral)
 *     Устраняет постоянную ошибку. Растет со временем.
 *     Anti-windup: если P+I >= max_limit, интегратор не растет.
 *     Предотвращает «раскачку» при резком изменении уставки.
 *
 *   ШАГ 5: Суммирование и ограничение
 *     output = P + I
 *     clamp(output, 0, max_limit)
 *
 *   ШАГ 6: Ограничение скорости (asymmetric slew rate)
 *     При нагреве (limiter растет): max +rate_warm за вызов
 *     При охлаждении (limiter падает): max -rate_cool за вызов
 *     Отношение 3:1 по умолчанию (быстро гасим, медленно возвращаем)
 *
 *   ШАГ 7: Финальное ограничение
 *     limiter = min(limiter, user_pwm)
 *     Гарантирует, что яркость не упадет ниже 0.
 *
 * Параметры:
 *   s       — состояние регулятора (обновляется)
 *   cfg     — конфигурация (не изменяется)
 *   adc_raw — сырые данные АЦП NTC (0-1023)
 *   user_pwm — заданная яркость (0-511, 9 бит)
 *
 * Возвращает:
 *   limiter (0..user_pwm) — величина ограничения мощности
 *   Итоговый ШИМ = user_pwm - limiter
 */
uint16_t ThermalLimiter_Update(ThermalLimiterState *s, const ThermalLimiterConfig *cfg,
                               uint16_t adc_raw, uint16_t user_pwm)
{
    int16_t error;
    int32_t P_term, I_term, output_signed;
    int32_t test_integral;
    uint16_t target, limiter;
    int error_sign;
    int error_positive; /* 1 = температура выше уставки (перегрев) */

    /* ====================================================================
     * ШАГ 1: IIR-фильтр низкой частоты
     * ====================================================================
     * Формула: filtered += (raw - filtered) >> filter_shift
     *
     * Это экспоненциальный скользящий средний (Exponential Moving Average).
     * При filter_shift = N, постоянная времени tau = 2^N выборок.
     *
     * Примеры:
     *   shift=4: tau=16 выборок = 0.53 сек  (быстрый, но шумный)
     *   shift=5: tau=32 выборки = 1.06 сек  (компромисс)
     *   shift=8: tau=256 выборок = 8.5 сек  (медленный, чистый)
     *
     * При(filtered_temp == 0 && prev_limiter == 0) — первый вызов:
     *   фильтр инициализируется сырым значением без задержки.
     *   Это предотвращает 1-секундный «стартовый транзиент».
     */
    if (s->filtered_temp == 0 && s->prev_limiter == 0) {
        s->filtered_temp = (int16_t)adc_raw;
    } else {
        s->filtered_temp += (int16_t)(((int16_t)adc_raw - s->filtered_temp) >> cfg->filter_shift);
    }

    /* ====================================================================
     * ШАГ 2: Вычисление ошибки регулирования
     * ====================================================================
     * Знак ошибки зависит от положения NTC в делителе:
     *
     *   NTC в верхнем плече (ntc_upper_leg = 1):
     *     V_ADC = Vdd * R_lower / (R_NTC + R_lower)
     *     R_NTC падает → V_ADC растет → ADC растет
     *     error = filtered - setpoint (перегрев → положительная ошибка)
     *
     *   NTC в нижнем плече (ntc_upper_leg = 0):
     *     V_ADC = Vdd * R_NTC / (R_NTC + R_lower)
     *     R_NTC падает → V_ADC падает → ADC падает
     *     error = setpoint - filtered (перегрев → положительная ошибка)
     *
     * После этого блока error >= 0 означает перегрев.
     */
    if (cfg->ntc_upper_leg) {
        error = s->filtered_temp - (int16_t)cfg->temp_setpoint;
    } else {
        error = (int16_t)cfg->temp_setpoint - s->filtered_temp;
    }

    /* ====================================================================
     * ШАГ 3: Пропорциональная составляющая (P)
     * ====================================================================
     * P_term = (|error| * Kp) >> TL_GAIN_SHIFT * sign(error)
     *
     * Формат Kp: Q8 (значение / 256).
     * Пример: Kp=4, error=100:
     *   P = (100 * 4) >> 8 = 400 / 256 = 1
     *
     * P обеспечивает быструю реакцию на ошибку, но не устраняет
     * постоянную ошибку (steady-state error). Для этого нужен I.
     */
    if (error >= 0) {
        error_sign = 1;
        error_positive = 1;
    } else {
        error_sign = -1;
        error_positive = 0;
        error = -error;  /* берём модуль для вычислений */
    }

    P_term = tl_div_round((int32_t)error * (int32_t)cfg->Kp, 1 << TL_GAIN_SHIFT);
    P_term *= error_sign;

    /* ====================================================================
     * ШАГ 4: Интегральная составляющая (I) + Anti-Windup
     * ====================================================================
     * Классическая PI-схема:
     *   - error > 0 (перегрев): integral += error
     *   - error <= 0 (недогрев/норма): integral decay к нулю
     *
     * Anti-Windup: I-вклад не должен превышать max_limit.
     */
    if (error_positive && error > 5) {
        /* Только при ошибке > 5 накапливаем интегратор (deadband) */
        /* Deadband: при малой ошибке (E < 5) не накапливаем */
        if (error > 5) {
            test_integral = s->integral + (int32_t)error;
            int32_t max_i_contribution = (int32_t)cfg->max_limit << TL_GAIN_SHIFT;
            if (test_integral * (int32_t)cfg->Ki <= max_i_contribution) {
                if (test_integral > cfg->I_max)
                    test_integral = cfg->I_max;
                s->integral = test_integral;
            }
        }
    } else {
        /* Быстрое стравливание при охлаждении */
        if (s->integral > 0) {
            int32_t decay = s->integral >> 5;
            if (decay == 0) decay = 1;
            s->integral -= decay;
            if (s->integral < 0) s->integral = 0;
        }
    }

    /* Вычисляем итоговую I-составляющую */
    I_term = tl_div_round(s->integral * (int32_t)cfg->Ki, 1 << TL_GAIN_SHIFT);

    /* Сохраняем P для отладки */
    s->p_term_out = P_term;

    /* ====================================================================
     * ШАГ 5: Суммирование P + I и ограничение
     * ==================================================================== */
    output_signed = P_term + I_term;

    if (output_signed < 0)
        output_signed = 0;
    if (output_signed > (int32_t)cfg->max_limit)
        output_signed = (int32_t)cfg->max_limit;

    target = (uint16_t)output_signed;

    /* ====================================================================
     * ШАГ 6: Ограничение скорости изменения (Asymmetric Slew Rate)
     * ====================================================================
     * Предотвращает резкие скачки яркости, даже при быстром
     * изменении температуры.
     *
     * Асимметрия:
     *   Нагрев (limiter растет, яркость падает): быстрое гашение
     *     rate_warm = 3 кода/вызов → ~90 кодов/сек
     *   Охлаждение (limiter падает, яркость растет): медленное восстановление
     *     rate_cool = 1 код/вызов → ~30 кодов/сек
     *
     * Соотношение 3:1 (по ТЗ: скорость восстановления в 2-3 раза меньше).
     *
     * Это исключает:
     *   - Видимое мерцание
     *   - Резкие изменения яркости
     *   - Автоколебания
     */
    if (s->prev_limiter < target) {
        /* limiter растет (уменьшаем яркость) — быстрое гашение */
        uint16_t delta = target - s->prev_limiter;
        if (delta > cfg->rate_warm)
            delta = cfg->rate_warm;
        limiter = s->prev_limiter + delta;
    } else if (s->prev_limiter > target) {
        /* limiter падает (увеличиваем яркость) — медленное восстановление */
        uint16_t delta = s->prev_limiter - target;
        if (delta > cfg->rate_cool)
            delta = cfg->rate_cool;
        limiter = s->prev_limiter - delta;
    } else {
        /* limiter не изменился */
        limiter = s->prev_limiter;
    }

    s->prev_limiter = limiter;

    /* ====================================================================
     * ШАГ 7: Финальное ограничение
     * ====================================================================
     * limiter <= user_pwm гарантируется всегда.
     * Это означает: PWM = user_pwm - limiter >= 0.
     * Яркость никогда не упадет ниже 0.
     */
    if (limiter > user_pwm)
        limiter = user_pwm;

    return limiter;
}