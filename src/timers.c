/*
 * timers.c — Таймер 2 (1 мс системный тик)
 *
 * Конфигурация (выводится из SYSCLK_HZ, см. proteus_config.h):
 *   APB1 timer clock = SYSCLK (на CH32V003 делителя APB нет)
 *   PSC = SYSCLK/1МГц - 1  → timer clock = 1 МГц (1 тик = 1 мкс)
 *   ARR = 999              → прерывание каждые 1000 тиков = 1 мс
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
#include "proteus_config.h"

/* Предделитель и период 1-мс тика. Промежуточная частота фиксирована на
 * 1 МГц, поэтому весь пересчёт при смене SYSCLK_HZ ложится на PSC. */
#define TIM2_TICK_HZ    1000000u
#define TIM2_PSC        ((SYSCLK_HZ / TIM2_TICK_HZ) - 1u)
#define TIM2_ARR        ((TIM2_TICK_HZ / 1000u) - 1u)

/* Тик обязан быть ровно 1 мс: от него зависят все опросы (160/30/2 Гц,
 * 5 с), пороги FSM кнопки в тиках и tim2_ms. */
_Static_assert((TIM2_PSC + 1u) * (TIM2_ARR + 1u) == SYSCLK_HZ / 1000u,
               "TIM2 prescaler/period do not yield a 1 ms tick");
/* PSC (CTLR/PSC) и ARR (ATRLR) — 16-битные регистры. */
_Static_assert(TIM2_PSC <= 0xFFFFu && TIM2_ARR <= 0xFFFFu,
               "TIM2 prescaler/period exceed the 16-bit registers");

volatile uint32_t tim2_ms = 0;

/* Интервалы таймеров (аналог millis в Arduino) */
#define TICK_160HZ_MS 6 /* 1000мс / 160Гц = 6.25мс → округляем до 6мс */
#define TICK_30HZ_MS 33 /* 1000мс / 30Гц = 33мс */
#define TICK_2HZ_MS 500 /* 1000мс / 2Гц = 500мс */
#define TICK_5S_MS 5000 /* 5 секунд */

/* Счётчики последнего срабатывания (static — доступны только из timers.c) */
static uint32_t last_tick_160hz = 0;
static uint32_t last_tick_30hz = 0;
static uint32_t last_tick_2hz = 0;
static uint32_t last_tick_5s = 0;

static inline bool check_interval(uint32_t *last, uint32_t interval)
{
    if (tim2_ms - *last >= interval)
    {
        *last += interval;
        return true;
    }
    return false;
}

bool tick_160hz(void) { return check_interval(&last_tick_160hz, TICK_160HZ_MS); }
bool tick_30hz(void)  { return check_interval(&last_tick_30hz, TICK_30HZ_MS); }
bool tick_2hz(void)   { return check_interval(&last_tick_2hz, TICK_2HZ_MS); }
bool tick_5s(void)    { return check_interval(&last_tick_5s, TICK_5S_MS); }

/**
 * @brief Сброс 5-секундного таймера.
 *
 * Перезапускает счётчик 5-секундного интервала. Вызывается при
 * активности кнопки, чтобы отложить переход в спящий режим.
 */
void tick_5s_reset(void)
{
    last_tick_5s = tim2_ms;
}

/**
 * @brief Busy-wait for ms milliseconds (approximate).
 * @param ms Milliseconds to wait (0..65535).
 * @details Заменяет SDK Delay_Ms()/Delay_Init(): не зависит от
 * SystemCoreClock и не тянет libgcc-арифметику. Число итераций
 * внутреннего volatile-цикла выводится из SYSCLK_HZ через
 * SPIN_LOOPS_MS(). Точность приблизительная — только для пауз
 * (boot, мигание индикатора).
 */
void wait_ms(uint16_t ms)
{
    const uint32_t loops = SPIN_LOOPS_MS(1);
    while (ms--)
    {
        volatile uint32_t cnt = loops;
        while (cnt--) ;
    }
}

/**
 * @brief Запуск таймера 2 с периодом 1 мс
 *
 * Выполняет полную инициализацию:
 *   1. Включает тактирование TIM2 (APB1 → RCC_TIM2EN)
 *   2. Настраивает NVIC (вектор 38, приоритет preemption=1, sub=1)
 *   3. Конфигурирует таймер: PSC/ARR из SYSCLK_HZ, прерывание Update
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
    TIM2->PSC = TIM2_PSC;
    TIM2->ATRLR = TIM2_ARR;
    TIM2->SWEVGR = TIM_PSCReloadMode_Immediate;
    /* INTFR — rc_w0: UG-событие выше взвело флаг UIF; сбрасываем его
     * ДО разрешения прерывания, иначе первый тик срабатывает паразитно
     * сразу после пуска счётчика. */
    TIM2->INTFR = 0;
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
    last_tick_30hz = 0;
    last_tick_2hz = 0;
    last_tick_5s = 0;
}

/**
 * @brief Обработчик прерывания TIM2 (вектор 38)
 *
 * Вызывается каждую 1 мс. Инкрементирует глобальный счётчик tim2_ms.
 *
 * Принято по аудиту (сентябрь 2026): UIF — липкий флаг без очереди, и при
 * задержке ISR дольше 1 мс два периода дают один pending-бит — tim2_ms
 * отстаёт на 1 мс за «проглоченный» тик. Задержки такой длительности
 * возможны только на операциях NVM: WL fast page erase (~1–3 мс, MIE
 * маскирован на весь erase в wl_flash_hal) и OB flush (несколько мс,
 * маскирован весь цикл в ob.c); вдобавок ядро стоит на выборке из Flash
 * всё время программирования независимо от масок. В повседневной работе
 * потерь нет вовсе. Потери НЕ накапливаются: все потребители tim2_ms —
 * дедлайн-логика с догоном (check_interval: last += interval), худший
 * эффект — однократное растяжение одного интервала на 1–3 мс.
 * Полный фикс (ISR в .highcode + снятие MIE-масок в vendored wl_flash_hal
 * и ob.c) признан непропорциональным риск/польза и не выполняется.
 */
void TIM2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void TIM2_IRQHandler(void)
{
    TIM2->INTFR = 0;
    tim2_ms++;
}
