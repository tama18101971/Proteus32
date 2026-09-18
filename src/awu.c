/*
 * awu.c — Реализация Auto Wake-Up таймера для CH32V003
 *
 * Функции:
 *   AWU_init()        — включение PWR тактирования и LSI генератора
 *   AWU_int_init()    — настройка EXTI Line9 + NVIC для обработки пробуждения
 *   AWU_set_delay()   — установка предделителя и окна по перечислению awu_tick_t
 *   AWU_set_raw()     — установка предделителя и окна напрямую (для AUX-автомата)
 *   AWU_Sleep_Now()   — вход в STANDBY; источник пробуждения — глобальная wakeup_source
 *   AWU_stop()        — полная остановка AWU + опционально LSI
 *
 * Регистры (прямая запись, без SDK-хелперов):
 *   PWR->AWUPSC  — предделитель (биты 3:0)
 *   PWR->AWUWR   — значение окна (биты 5:0)
 *   PWR->AWUCSR  — управление AWU (бит 1: AWUEN)
 *   PWR->CTLR    — управление питанием (бит 1: PDDS — STANDBY mode)
 *
 * Примечание: после AWU_stop() регистры теряются (тактирование отключено).
 */

#include "awu.h"
#include "proteus_config.h"
#include "mini_printf.h"

volatile uint8_t is_sleep_mode = false;

#if USE_LSI_GUARD
/* Результат последнего ожидания стабилизации LSI в AWU_init(): true —
 * генератор работает (LSIRDY=1), false — истёк таймаут ожидания.
 * Гейты термосна и старта AUX-команд запрещают AWU-зависимые режимы,
 * пока флаг false: без LSI таймер AWU не тикает, и строб иначе остался
 * бы на первом ON-полупериоде до разряда АКБ. Восстанавливается каждой
 * попыткой AWU_init() (при каждом пробуждении кнопкой). */
bool awu_ready = true;
#endif

#ifdef UART_DEBUG
/**
 * @brief Ограниченное ожидание завершения передачи UART (флаг TC).
 *
 * Общая точка для пробуждения по кнопке и входа в STANDBY: сбрасывает
 * последний байт перед потерей тактирования USART1. Цикл ограничен
 * UART_TC_WAIT_LOOPS (~1 мс, выводится из SYSCLK_HZ), чтобы неисправный
 * или незаклокированный USART не подвесил ISR. noinline — чтобы LTO не
 * дублировал цикл.
 */
__attribute__((noinline)) void uart_tc_wait(void)
{
    uint32_t _w = UART_TC_WAIT_LOOPS;
    while (!(USART1->STATR & USART_STATR_TC))
        if (!--_w) break;
}
#endif

/* ============================================================
 * AWU TABLE
 * ============================================================
 * LSI = 128 kHz. Formula: Time = (Prescaler * WindowValue) / 128000
 */
typedef struct
{
    uint8_t psc;    /* Prescaler value (4 bits for AWUPSC register) */
    uint8_t window; /* Window value (6 bits for AWUWR register) */
} awu_cfg_t;

static const awu_cfg_t awu_lut[] = {
    {PWR_AWUPSC_128, 1},    /* AWU_1MS   = (128 * 1) / 128000 = 0.001s */
    {PWR_AWUPSC_128, 15},   /* AWU_15MS  = (128  * 15) / 128000 = 0.015s  */
    {PWR_AWUPSC_128, 30},   /* AWU_30MS  = (128  * 30) / 128000 = 0.030s  */
    {PWR_AWUPSC_256, 30},   /* AWU_60MS  = (256  * 30) / 128000 = 0.060s  */
    {PWR_AWUPSC_256, 60},   /* AWU_120MS = (256  * 60) / 128000 = 0.120s  */
    {PWR_AWUPSC_512, 63},   /* AWU_250MS = (512  * 63) / 128000 = 0.252s  */
    {PWR_AWUPSC_1024, 63},  /* AWU_500MS = (1024 * 63) / 128000 = 0.504s  */
    {PWR_AWUPSC_2048, 63},  /* AWU_1S    = (2048 * 63) / 128000 = 1.008s  */
    {PWR_AWUPSC_10240, 25}, /* AWU_2S    = (10240* 25) / 128000 = 2.0s    */
    {PWR_AWUPSC_10240, 50}, /* AWU_4S    = (10240* 50) / 128000 = 4.0s    */
    {PWR_AWUPSC_61440, 17}, /* AWU_8S    = (61440* 17) / 128000 = 8.16s   */
    {PWR_AWUPSC_61440, 33}, /* AWU_16S   = (61440* 33) / 128000 = 16.32s  */
};

#define AWU_TICKS_COUNT (sizeof(awu_lut) / sizeof(awu_lut[0]))

/* ============================================================
 * AWU_set_delay — настройка prescaler и window
 * ============================================================ */
/**
 * @brief Установка задержки AWU по индексу из таблицы.
 * @param awu_tick Индекс интервала (AWU_15MS..AWU_16S).
 * @return true если индекс валиден, false если выходит за границы таблицы.
 */
bool AWU_set_delay(awu_tick_t awu_tick)
{
    if (awu_tick >= AWU_TICKS_COUNT)
        return false;

    PWR->AWUPSC = awu_lut[awu_tick].psc;   /* Запись предделителя (биты 3:0 регистра AWUPSC) */
    PWR->AWUWR = awu_lut[awu_tick].window; /* Запись значения окна (биты 5:0 регистра AWUWR) */

    return true;
}

/**
 * @brief Установка AWU напрямую по индексу предделителя и значению окна.
 * @param psc_idx Индекс предделителя (0..7 → PWR_AWUPSC 0x08..0x0F, делители 128..61440).
 * @param win     Значение окна (0..63 → PWR_AWUWR).
 */
void AWU_set_raw(uint8_t psc_idx, uint8_t win)
{
    PWR->AWUPSC = psc_idx + 0x08;
    PWR->AWUWR = win;
}

/**
 * @brief Настройка прерывания AWU (EXTI Line9 + NVIC).
 *
 * Включает тактирование AFIO, настраивает EXTI Line9 на rising edge
 * и разрешает прерывание AWU в PFIC (вектор 21).
 */
void AWU_int_init(void)
{
    /* Включаем тактирование AFIO (обязательно для EXTI) */
    RCC->APB2PCENR |= RCC_AFIOEN;

    /* EXTI Line9 — interrupt mode, rising trigger
     * AWU внутренне подключён к EXTI9 (RM табл. 6-2) */
    EXTI->INTENR |= (1 << 9); /* Маска прерывания для Line 9 */
    EXTI->RTENR  |= (1 << 9);  /* Rising trigger для Line 9 */

    /* PFIC — включаем AWU IRQ (вектор 21) */
    NVIC->IENR[0] = (1 << 21);
    NVIC->IPRIOR[21] = (1 << 7) | (1 << 6); /* preemption=1, sub=1 */
}

/**
 * @brief Включение тактирования PWR и LSI генератора.
 *
 * Включает тактирование PWR (для доступа к регистрам управления питанием)
 * и запускает LSI (128 kHz), необходимый для работы AWU. Ждёт стабилизации LSI.
 */
void AWU_init(void)
{
    RCC->APB1PCENR |= RCC_PWREN; /* Включаем тактирование PWR (бит 28 APB1PCENR) */
    RCC->RSTSCKR |= RCC_LSION;   /* Включаем LSI генератор (бит 0 RSTSCKR) */

    /* Ждём стабилизации LSI (бит 1 RSTSCKR) с таймаутом: отказ генератора
     * не должен вешать boot до старта остальных подсистем (~120 мс при
     * HSI 8 МГц до настройки PLL). */
    uint32_t timeout = 1000000u;
    while (!(RCC->RSTSCKR & RCC_LSIRDY))
        if (!--timeout)
            break;

#if USE_LSI_GUARD
    /* Таймаут — не «продолжаем как ни в чём не бывало»: публикуем статус.
     * Каждое пробуждение кнопкой заново выполняет AWU_init(), поэтому
     * транзиентный отказ (EMI, холодный старт) само-восстанавливается
     * без выделенного ретрай-цикла. */
    awu_ready = (RCC->RSTSCKR & RCC_LSIRDY) != 0u;
#endif
}

/**
 * @brief Полная остановка AWU и опционально LSI.
 * @param stop_lsi Если true, также выключает LSI-генератор.
 *
 * Отключает AWU (AWUEN=0). При stop_lsi=true выключает и LSI,
 * экономя энергию в режиме сна.
 */
void AWU_stop(bool stop_lsi)
{
    PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN; /* Выключаем AWU (бит 1 AWUCSR) */

    if (stop_lsi)
    {
        RCC->RSTSCKR &= ~RCC_LSION; /* Выключаем LSI (бит 0 RSTSCKR) */
    }
}

/**
 * @brief Вход в STANDBY и пробуждение.
 *
 * Устанавливает PDDS=1 для STANDBY, выполняет WFI.
 * Пробуждение происходит по EXTI (кнопка) или AWU-событию.
 * После пробуждения сбрасывает PDDS и SLEEPDEEP.
 * Защита от lost wakeup: прерывания маскируются на время проверки
 * wakeup_source/__WFI(), поэтому нажатие кнопки в окне входа в сон
 * либо не даёт уснуть (событие уже в wakeup_source), либо остаётся
 * pending и будит ядро из WFI. Источник пробуждения — глобальная
 * wakeup_source (WS_NONE, WS_BUTTON или WS_AWU) по возврату.
 * Если UART_DEBUG включён — ждёт завершения UART перед сном
 * и переинициализирует UART после пробуждения.
 */
void AWU_Sleep_Now(void)
{
#ifdef UART_DEBUG
    uart_tc_wait(); /* Сбросить последний байт перед потерей тактирования USART1 */
#endif

    is_sleep_mode = true; /* Устанавливаем флаг сна */

    PWR->CTLR |= PWR_CTLR_PDDS; /*Устанавливаем PDDS=1 в PWR_CTLR → выбираем Standby (не Sleep) */

    /* CH32V003 uses PFIC->SCTLR bit 2 as SLEEPDEEP.  PDDS alone selects the
     * standby power mode but WFE would otherwise remain in light sleep. */
    PFIC->SCTLR |= (1 << 2);

    /* Барьер: убедиться, что все записи завершены */
    __NOP(); __NOP(); __NOP();

    /* 4. WFI — войти в Standby без потери нажатия (lost wakeup race).
     *    Маскируем MIE и атомарно читаем wakeup_source:
     *    - событие, пришедшее ДО маскирования, уже записано ISR в
     *      wakeup_source: не спим (кнопку/тик main потребит в штатном
     *      порядке — следующий вызов увидит WS_NONE), поэтому тик не
     *      теряется и busy-spin не возникает;
     *    - событие, пришедшее ПОСЛЕ чтения, остаётся pending в PFIC и
     *      будит ядро из WFI (wfi выходит по pending разрешённого
     *      прерывания независимо от MIE); ISR исполнится сразу после
     *      повторного разрешения MIE ниже. */
    __asm volatile("csrci mstatus, 8" ::: "memory");

    wakeup_source_t pre = wakeup_source;

    if (pre == WS_NONE)
    {
        __WFI();
    }

    /* Снимаем SLEEPDEEP и PDDS ДО повторного разрешения прерываний, чтобы
     * ISR, исполненный сразу после csrsi, не застал ядро в состоянии
     * "готов к Standby" (иначе паразитный WFI из главного цикла заснул бы
     * насмерть вместо Light Sleep). */
    PFIC->SCTLR &= ~(1 << 2);
    PWR->CTLR &= ~PWR_CTLR_PDDS;

    __asm volatile("csrsi mstatus, 8" ::: "memory");

#ifdef UART_DEBUG
    uart_init(UART_DEBUG_BAUD);
#endif

    return;  /* wakeup_source содержит WS_AWU, WS_BUTTON или WS_NONE */
}

/**
 * @brief Обработчик прерывания AWU (вектор 21).
 *
 * Вызывается при пробуждении от AWU-события (EXTI Line9).
 * Устанавливает wakeup_source = WS_AWU и очищает флаг EXTI9 прямой записью
 * в регистр EXTI->INTFR, без вызовов SDK.
 *
 * Восстановление системных часов здесь НЕ выполняется: при выходе из
 * STANDBY аппаратура сама возвращает SYSCLK на HSI 24 МГц — ровно ту
 * конфигурацию, с которой работает прошивка (HPRE=DIV1, PLL выключен).
 * Прежний вызов SystemInit() из ISR попутно сбрасывал поле ADCPRE в
 * RCC->CFGR0, настроенное adc_init(); тактирование АЦП корректно
 * восстанавливается adc_init() в main до первого измерения.
 */
void AWU_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void AWU_IRQHandler(void)
{
    /* EXTI_GetITStatus(): pending flag AND interrupt mask for AWU line 9. */
    if ((EXTI->INTFR & EXTI->INTENR & (1U << 9)) != 0U)
    {
        /* Кнопка и AWU могут стать pending одновременно сразу после __WFI().
         * Не затираем уже установленный WS_BUTTON: нажатие — видимый
         * пользователю источник пробуждения и имеет приоритет над тиком. */
        if (wakeup_source != WS_BUTTON)
            wakeup_source = WS_AWU;

        /* Очищаем pending-флаг EXTI9: CH32V003 clears it by writing 1. */
        EXTI->INTFR = (1U << 9);
    }
}
