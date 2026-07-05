/*
 * soft_start.h — плавный soft start для регулировки яркости
 *
 * Модуль обеспечивает плавное нарастание/убывание яркости (step)
 * от текущего значения до целевого за заданное количество шагов.
 * Вызывается из timer interrupt или main loop через ss_scan().
 *
 * Алгоритм:
 * 1. ss_init() — вычисляет шаг, количество шагов и направление
 * 2. ss_scan() — каждый вызов перемещает позицию на один шаг к цели
 * 3. При достижении цели — ss.enable = false, позиция = target
 */

#ifndef INC_SOFT_START_H_
#define INC_SOFT_START_H_

#include "stdint.h"

/* Макрос модуля (абсолютное значение). Не parenthesizes аргумент —
 * безопасно только для простых выражений (переменные, константы).
 * Для сложных выражений используйте скобки: ABS((a - b)) */
#define ABS(x)				((x>0)?(x):(-x))

/* Скорость soft start (шагов за вызов ss_scan).
 * SS_NO   = 0 — без плавного нарастания (мгновенный переход)
 * SS_SLOW = 1 — медленное нарастание (1 шаг за вызов)
 * SS_FAST = 8 — быстрое нарастание (8 шагов за вызов) */
typedef enum {
	SS_NO = 0,
	SS_SLOW,
	SS_FAST = 7
} SoftStartSpeed_t;

#define on_ss() do { ss_enable = true; } while (0);

/* Структура состояния soft start.
 * Заполняется в ss_init(), используется в ss_scan(). */
typedef struct {
	uint8_t enable;         /* Флаг активности: 1=.soft start работает, 0=завершён */
	int8_t step_size;       /* Размер шага: +speed при ramp-up, -speed при ramp-down */
	uint16_t target;        /* Целевое значение яркости (куда идём) */
	uint16_t step_count;    /* Оставшееся количество шагов (уменьшается в ss_scan) */
	uint16_t position;      /* Текущая позиция яркости (обновляется в ss_scan) */
} ss_st;

void ss_scan(void);                                              /* Вызывается каждый тик — двигает позицию к цели */
void ss_init(uint16_t target, uint16_t previous, SoftStartSpeed_t speed); /* Инициализация soft start */

#endif /* INC_SOFT_START_H_ */
