#include "awu.h"
#include <stdint.h>
#include <stdbool.h>
#include "ch32v00x.h"
#include "system_ch32v00x.h"
#include "debug.h"
#include "config.h"
#include "button_gpio.h"

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
bool AWU_set_delay(awu_tick_t awu_tick)
{
    if (awu_tick >= AWU_TICKS_COUNT)
        return false;

    PWR->AWUPSC = awu_lut[awu_tick].psc;   /* Запись предделителя (биты 3:0 регистра AWUPSC) */
    PWR->AWUWR = awu_lut[awu_tick].window; /* Запись значения окна (биты 5:0 регистра AWUWR) */

    return true;
}

/* Установка AWU напрямую по индексу предделителя и значению окна.
 * psc_idx: 0..7 → PWR_AWUPSC 0x08..0x0F (делители 128..61440)
 * win:     0..63 → PWR_AWUWR (значение окна) */
void AWU_set_raw(uint8_t psc_idx, uint8_t win)
{
    PWR->AWUPSC = psc_idx + 0x08;
    PWR->AWUWR = win;
}

/* ============================================================
 * AWU_int_init — настройка EXTI Line 9 (AWU event)
 * ============================================================ */
void AWU_int_init(void)
{
    /* Включаем тактирование AFIO (обязательно для EXTI) */
    RCC->APB2PCENR |= RCC_AFIOEN;

    /* EXTI Line9 — interrupt mode, rising trigger
     * AWU внутренне подключён к EXTI9 (RM табл. 6-2) */
    EXTI->INTENR |= (1 << 9);   /* Маска прерывания для Line 9 */
    EXTI->RTENR  |= (1 << 9);   /* Rising trigger для Line 9 */

    /* PFIC — включаем AWU IRQ (вектор 21) */
    NVIC->IENR[0] = (1 << 21);
    NVIC->IPRIOR[21] = (1 << 7) | (1 << 6);  /* preemption=1, sub=1 */
}

/* ============================================================
 * AWU_init — включение PWR тактирования и LSI генератора
 * ============================================================ */
void AWU_init(void)
{
    RCC->APB1PCENR |= RCC_PWREN; /* Включаем тактирование PWR (бит 28 APB1PCENR) */
    RCC->RSTSCKR |= RCC_LSION;   /* Включаем LSI генератор (бит 0 RSTSCKR) */

    while (!(RCC->RSTSCKR & RCC_LSIRDY)) /* Ждём стабилизации LSI (бит 1 RSTSCKR) */
        ;
}

/* ============================================================
 * AWU_stop — выключение AWU и опционально LSI
 * ============================================================ */
void AWU_stop(bool stop_lsi)
{
    PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN; /* Выключаем AWU (бит 1 AWUCSR) */

    if (stop_lsi)
    {
        RCC->RSTSCKR &= ~RCC_LSION; /* Выключаем LSI (бит 0 RSTSCKR) */
    }
}

/* ============================================================
 * AWU_Sleep_Tick — настройка + сон
 * ============================================================ */
void AWU_Sleep_Tick(awu_tick_t tick)
{
    AWU_set_delay(tick);
    AWU_Sleep_Now();
}

/* ============================================================
 * AWU_Sleep_Now — вход в STANDBY и пробуждение
 * ============================================================ */
void AWU_Sleep_Now(void)
{
#ifdef WL_FLASH_DEBUG
    while (!(USART1->STATR & USART_FLAG_TC)) /* Ждём окончания передачи USART1 (бит 6 STATR) */
        ;
#endif

    //PWR->AWUCSR |= PWR_AWUCSR_AWUEN; /* Включаем AWU (таймер стартует) */

    PWR->CTLR |= PWR_CTLR_PDDS; /*Устанавливаем PDDS=1 в PWR_CTLR → выбираем Standby (не Sleep) */

    /* Барьер: убедиться, что все записи завершены */
    __NOP();  __NOP();  __NOP();

    /* 4. WFI — войти в Standby.
     *    Пробуждение: EXTI прерывание (кнопка) → EXTI7_0_IRQHandler
     *                 AWU событие              → AWU_IRQHandler
     *    Сброса не происходит (если STANDBY_RST в option bytes = 0) */
    __WFI();
    /*
     * Сюда выполнение попадёт ТОЛЬКО если пробуждение произошло,
     * но прерывание уже обработано. В нормальном сценарии
     * управление передаётся прямо в IRQ-обработчик.
     * После обработчика — возврат сюда, затем снова в Standby.
     */

    /* Снимаем SLEEPDEEP и PDDS — на случай если хотим продолжить работу */
    PFIC->SCTLR &= ~(1 << 2);
    PWR->CTLR &= ~PWR_CTLR_PDDS;

    //PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN; /* Выключаем AWU после пробуждения */

#ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200); /* Восстанавливаем UART для отладки */
#endif
}

/* ═══════════════════════════════════════════════════════════════
 * Обработчик прерывания: пробуждение по AWU (таймер ~5 с)
 *
 * Вектор AWU (таблица 6-1, номер 21, адрес 0x00000054)
 * AWU внутри подключён к EXTI Line9 (таблица 6-2)
 * ════════════════════════════════════════════════════════════════ */
void AWU_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void AWU_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line9) != RESET)
    {
        SystemInit();            /* Восстанавливаем системные часы после пробуждения */
        SystemCoreClockUpdate(); /* Обновляем переменную SystemCoreClock */

        EE_PRINTF("[AWU] WAKEUP Source: AWU TIMER\r\n");
        #ifdef WL_FLASH_DEBUG
        /* Ждём завершения передачи UART */
        while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {}
        #endif

        awu_event = 1;
        wakeup_source = WS_AWU;

        /* Очищаем флаг EXTI9 */
        EXTI_ClearITPendingBit(EXTI_Line9);

    }
}
