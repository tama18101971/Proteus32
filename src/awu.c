#include "awu.h"
#include <stdint.h>
#include <stdbool.h>
#include "system_ch32v00x.h"
#include "debug.h"
#include "config.h"

/* ============================================================
 * REGISTER DEFINITIONS (CH32V003 — only PWR AWU registers)
 * ============================================================
 * RCC, EXTI, USART registers — already defined in ch32v00x.h
 * (included via system_ch32v00x.h). We only define PWR AWU
 * registers that are NOT in the SDK header.
 */

/* --- PWR AWU registers (not in ch32v00x.h as struct members) --- */
#define PWR_CTLR            (*(volatile uint32_t *)(PWR_BASE + 0x00))
#define PWR_AWUCSR_REG      (*(volatile uint32_t *)(PWR_BASE + 0x08))
#define PWR_AWUWR_REG       (*(volatile uint32_t *)(PWR_BASE + 0x0C))
#define PWR_AWUPSC_REG      (*(volatile uint32_t *)(PWR_BASE + 0x10))

/*
 * PWR_BASE, PWR_CTLR_PDDS, PWR_AWUCSR_AWUEN, PWR_AWUPSC_128..61440
 * — определены в ch32v00x.h
 */

/* ============================================================
 * AWU TABLE
 * ============================================================
 * LSI = 128 kHz. Formula: Time = (Prescaler * WindowValue) / 128000
 */
typedef struct {
    uint8_t psc;    /* Prescaler value (4 bits for AWUPSC register) */
    uint8_t window; /* Window value (6 bits for AWUWR register) */
} awu_cfg_t;

static const awu_cfg_t awu_lut[] = {
    {PWR_AWUPSC_128,  15},  /* AWU_15MS  = (128  * 15) / 128000 = 0.015s  */
    {PWR_AWUPSC_128,  30},  /* AWU_30MS  = (128  * 30) / 128000 = 0.030s  */
    {PWR_AWUPSC_256,  30},  /* AWU_60MS  = (256  * 30) / 128000 = 0.060s  */
    {PWR_AWUPSC_256,  60},  /* AWU_120MS = (256  * 60) / 128000 = 0.120s  */
    {PWR_AWUPSC_512,  63},  /* AWU_250MS = (512  * 63) / 128000 = 0.252s  */
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

    PWR_AWUPSC_REG = awu_lut[awu_tick].psc;  /* Запись предделителя (биты 3:0 регистра AWUPSC) */
    PWR_AWUWR_REG  = awu_lut[awu_tick].window;  /* Запись значения окна (биты 5:0 регистра AWUWR) */

    return true;
}

/* ============================================================
 * AWU_int_init — настройка EXTI Line 9 (AWU event)
 * ============================================================ */
void AWU_int_init(void)
{
    RCC->APB2PCENR |= RCC_AFIOEN;            /* Включаем тактирование AFIO (обязательно для EXTI) */

    EXTI->EVENR  |=  EXTI_Line9;             /* Line 9 как EVENT (не interrupt) — пробуждение без обработчика */
    EXTI->RTENR  &= ~EXTI_Line9;             /* Сброс rising trigger для Line 9 */
    EXTI->FTENR  |=  EXTI_Line9;             /* Falling trigger для Line 9 (активный уровень — LOW) */
}

/* ============================================================
 * AWU_init — включение PWR тактирования и LSI генератора
 * ============================================================ */
void AWU_init(void)
{
    RCC->APB1PCENR |= RCC_PWREN;             /* Включаем тактирование PWR (бит 28 APB1PCENR) */
    RCC->RSTSCKR   |= RCC_LSION;             /* Включаем LSI генератор (бит 0 RSTSCKR) */

    while (!(RCC->RSTSCKR & RCC_LSIRDY))     /* Ждём стабилизации LSI (бит 1 RSTSCKR) */
        ;
}

/* ============================================================
 * AWU_stop — выключение AWU и опционально LSI
 * ============================================================ */
void AWU_stop(bool stop_lsi)
{
    PWR_AWUCSR_REG &= ~PWR_AWUCSR_AWUEN;    /* Выключаем AWU (бит 1 AWUCSR) */

    if (stop_lsi) {
        RCC->RSTSCKR &= ~RCC_LSION;        /* Выключаем LSI (бит 0 RSTSCKR) */
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
#if defined(SLEEP_STANDBY) && defined(WL_FLASH_DEBUG)
    while (!(USART1->STATR & USART_FLAG_TC))  /* Ждём окончания передачи USART1 (бит 6 STATR) */
        ;
#endif

    PWR_AWUCSR_REG |= PWR_AWUCSR_AWUEN;           /* Включаем AWU (таймер стартует) */
    PWR_CTLR       |= PWR_CTLR_PDDS;              /* Выбираем STANDBY режим (бит 1 CTLR = PDDS) */
    __WFE();                                       /* Wait For Event — MCU засыпает до события AWU */
    PWR_AWUCSR_REG &= ~PWR_AWUCSR_AWUEN;          /* Выключаем AWU после пробуждения */

    SystemInit();                              /* Восстанавливаем системные часы после пробуждения */
    SystemCoreClockUpdate();                   /* Обновляем переменную SystemCoreClock */
    Delay_Init();                              /* Инициализация Delay_us/Delay_ms */

#ifdef WL_FLASH_DEBUG
    USART_Printf_Init(115200);                 /* Восстанавливаем UART для отладки */
#endif
}
