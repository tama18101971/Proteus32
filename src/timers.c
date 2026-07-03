/*
 * timers.c — Таймер 2 (1 мс системный тик)
 *
 * Конфигурация:
 *   SYSCLK = 48 МГц (HSI + PLL)
 *   APB1 timer clock = 48 МГц (нет делителя на CH32V003)
 *   PSC = 47 → timer clock = 48 МГц / 48 = 1 МГц (1 тик = 1 мкс)
 *   ARR = 999 → прерывание каждые 1000 тиков = 1 мс
 *
 * Использование:
 *   tim2_start()  — полная инициализация + запуск таймера
 *   tim2_stop()   — полная остановка + отключение тактирования (экономия энергии)
 *   tim2_ms       — глобальный счётчик миллисекунд (инкрементируется в IRQHandler)
 *
 * Примечание: после tim2_stop() регистры TIM2 сбрасываются (отключено тактирование),
 * поэтому tim2_start() заново конфигурирует все регистры.
 */

#include "timers.h"
#include "ch32v00x.h"

volatile uint32_t tim2_ms = 0;

/* Интервалы таймеров (аналог millis в Arduino) */
#define TICK_160HZ_MS 6 /* 1000мс / 160Гц = 6.25мс → округляем до 6мс */
#define TICK_50HZ_MS 20 /* 1000мс / 50Гц = 20мс */
#define TICK_2HZ_MS 500 /* 1000мс / 2Гц = 500мс */
#define TICK_5S_MS 5000 /* 5 секунд */

/* Счётчики последнего срабатывания (static — доступны только из timers.c) */
static uint32_t last_tick_160hz = 0;
static uint32_t last_tick_50hz = 0;
static uint32_t last_tick_2hz = 0;
static uint32_t last_tick_5s = 0;

/**
 * @brief Проверка: прошло ли TICK_160HZ_MS (6 мс) с последнего вызова
 * @return true если интервал истёк (счётчик автоматически сдвигается)
 *
 * Использование:
 *   if (tick_160hz()) { ... }
 */
bool tick_160hz(void)
{
    if (tim2_ms - last_tick_160hz >= TICK_160HZ_MS)
    {
        last_tick_160hz += TICK_160HZ_MS;
        return true;
    }
    return false;
}

/**
 * @brief Проверка: прошло ли TICK_50HZ_MS (20 мс) с последнего вызова
 * @return true если интервал истёк (счётчик автоматически сдвигается)
 */
bool tick_50hz(void)
{
    if (tim2_ms - last_tick_50hz >= TICK_50HZ_MS)
    {
        last_tick_50hz += TICK_50HZ_MS;
        return true;
    }
    return false;
}

/**
 * @brief Проверка: прошло ли TICK_2HZ_MS (500 мс) с последнего вызова
 * @return true если интервал истёк (счётчик автоматически сдвигается)
 */
bool tick_2hz(void)
{
    if (tim2_ms - last_tick_2hz >= TICK_2HZ_MS)
    {
        last_tick_2hz += TICK_2HZ_MS;
        return true;
    }
    return false;
}

/**
 * @brief Проверка: прошло ли TICK_5S_MS (5000 мс) с последнего вызова
 * @return true если интервал истёк (счётчик автоматически сдвигается)
 */
bool tick_5s(void)
{
    if (tim2_ms - last_tick_5s >= TICK_5S_MS)
    {
        last_tick_5s += TICK_5S_MS;
        return true;
    }
    return false;
}

void tick_5s_reset(void)
{
    last_tick_5s = tim2_ms;
}

/**
 * @brief Запуск таймера 2 с периодом 1 мс
 *
 * Выполняет полную инициализацию:
 *   1. Включает тактирование TIM2 (APB1 → RCC_TIM2EN)
 *   2. Настраивает NVIC (вектор 38, приоритет preemption=1, sub=1)
 *   3. Конфигурирует таймер: PSC=47, ARR=999, прерывание Update
 *   4. Запускает счётчик
 */
void tim2_start(void)
{
    if (TIM2->CTLR1 & (1 << 0))
        return;

    RCC->APB1PCENR |= RCC_TIM2EN;

    NVIC->IENR[38 >> 5] = (1 << (38 & 0x1F));
    NVIC->IPRIOR[38] = (1 << 7) | (1 << 6);

    TIM2->CTLR1 = 0;
    TIM2->INTFR = 0;
    TIM2->PSC = 48 - 1;
    TIM2->ATRLR = 1000 - 1;
    TIM2->SWEVGR = TIM_PSCReloadMode_Immediate;
    TIM2->DMAINTENR = TIM_IT_Update;

    TIM2->CNT = 0;
    TIM2->CTLR1 |= (1 << 0);
}

/**
 * @brief Полная остановка таймера 2 с отключением тактирования
 */
void tim2_stop(void)
{
    if (!(TIM2->CTLR1 & (1 << 0)))
        return;

    /* отключаем таймер */
    TIM2->CTLR1 &= ~(1 << 0);
    TIM2->DMAINTENR = 0;
    NVIC->IRER[38 >> 5] = (1 << (38 & 0x1F));
    RCC->APB1PCENR &= ~RCC_TIM2EN;

    /* обнулям счетчики */
    tim2_ms = 0;
    last_tick_160hz = 0;
    last_tick_50hz = 0;
    last_tick_2hz = 0;
    last_tick_5s = 0;
}

/**
 * @brief Обработчик прерывания TIM2 (вектор 38)
 *
 * Вызывается каждую 1 мс. Инкрементирует глобальный счётчик tim2_ms.
 */
void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM2_IRQHandler(void)
{
    TIM2->INTFR = 0;
    tim2_ms++;
}
