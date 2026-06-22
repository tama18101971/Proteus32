#ifndef PID_H
#define PID_H

#include <stdint.h>

/*
 * ПИД-регулятор для CH32V003 (RISC-V, нет FPU)
 * Вход:  10 бит  (0..1023)   — например, АЦП
 * Выход: 12 бит  (0..4095)   — например, ШИМ
 *
 * Арифметика целочисленная, фиксированная точка Q SHIFT бит,
 * чтобы избежать float (на CH32V003 он эмулируется программно и жрёт такты).
 */

#define PID_SHIFT      8                 // дробная часть коэффициентов (Q8)
#define PID_SCALE      (1 << PID_SHIFT)  // = 256

#define PID_IN_MAX     1023              // 10 бит
#define PID_OUT_MAX    4095              // 12 бит

typedef struct {
    /* настраиваемые коэффициенты, формат Q8 (реальный_Kp * 256) */
    int32_t Kp;
    int32_t Ki;
    int32_t Kd;

    int32_t setpoint;     // заданное значение (в шкале входа, 0..1023)

    int32_t integral;     // накопленный интеграл (уже с учётом Ki, ограничен)
    int32_t prev_input;   // предыдущее измерение (для D по входу, без выбросов)

    int32_t out_min;      // ограничение выхода, обычно 0
    int32_t out_max;      // ограничение выхода, обычно PID_OUT_MAX
} PID_t;

/* Инициализация. kp,ki,kd передаются уже в формате Q8 (см. PID_F2Q ниже) */
void PID_Init(PID_t *pid, int32_t kp, int32_t ki, int32_t kd, int32_t setpoint);

/* Установка коэффициентов "на ходу" (например, по UART) */
static inline void PID_SetTunings(PID_t *pid, int32_t kp, int32_t ki, int32_t kd)
{
    pid->Kp = kp;
    pid->Ki = ki;
    pid->Kd = kd;
}

static inline void PID_SetSetpoint(PID_t *pid, int32_t setpoint)
{
    pid->setpoint = setpoint;
}

/* Сброс интегральной и дифференциальной памяти (при старте/смене режима) */
void PID_Reset(PID_t *pid, int32_t current_input);

/*
 * Основной шаг регулятора. Вызывать с фиксированным периодом Ts.
 * input — текущее измеренное значение, 0..1023 (10 бит)
 * Возвращает управляющее воздействие, 0..4095 (12 бит)
 */
int32_t PID_Compute(PID_t *pid, int32_t input);

/* Перевод "человеческого" коэффициента (например 2.5) в формат Q8 на этапе компиляции */
#define PID_F2Q(x)  ((int32_t)((x) * PID_SCALE))

#endif /* PID_H */
