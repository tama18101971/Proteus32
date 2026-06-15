#include "awu.h"
#include <stdint.h>
#include <stdbool.h>
#include "ch32v00x_pwr.h"

// Структура для таблицы настроек AWU
typedef struct
{
    uint32_t prescaler;
    uint8_t window;
} awu_cfg_t;

/*----------------------------------------------------------------------------------*/
// * Преобразует перечисление временных интервалов awu_tick_t в параметры AWU:
// * - Prescaler: предделитель частоты LSI (128 kHz на CH32V003)
// * - WindowValue: значение окна сравнения (0-63), определяет длительность сна
// *
// * Формула времени сна на CH32V003:
// *   Time(sleep) = (Prescaler × WindowValue) / LSI_Freq
// *
// * Где:
// *   LSI_Freq = 128000 Гц (внутренний генератор)
// *   Prescaler - коэффициент из регистра AWU (128, 256, 512, 1024, 2048, 10240, 61440)
// *   WindowValue - значение счетчика AWU (0-63)
// *
// * Алгебраические расчеты для выбранных значений:
// *   15ms  = (128 × 15) / 128000 = 0.015 с
// *   30ms  = (128 × 30) / 128000 = 0.03 с
// *   60ms  = (256 × 30) / 128000 = 0.06 с
// *   120ms = (256 × 60) / 128000 = 0.12 с
// *   250ms = (512 × 63) / 128000 = 0.252 с
// *   500ms = (1024 × 63)/ 128000 = 0.504 с
// *   1s    = (2048 × 63)/ 128000 = 1.008 с
// *   2s    = (10240× 25)/ 128000 = 2.0 с
// *   4s    = (10240× 50)/ 128000 = 4.0 с
// *   8s    = (61440× 17)/ 128000 = 8.16 с
// *   16s   = (61440× 33)/ 128000 = 16.32 с
/*----------------------------------------------------------------------------------*/

// Таблица поиска (размещается в Flash-памяти благодаря const)
static const awu_cfg_t awu_lut[] = {
    {PWR_AWU_Prescaler_128, 15},   // AUX_15MS
    {PWR_AWU_Prescaler_128, 30},   // AUX_30MS
    {PWR_AWU_Prescaler_256, 30},   // AUX_60MS
    {PWR_AWU_Prescaler_256, 60},   // AUX_120MS
    {PWR_AWU_Prescaler_512, 63},   // AUX_250MS
    {PWR_AWU_Prescaler_1024, 63},  // AUX_500MS
    {PWR_AWU_Prescaler_2048, 63},  // AUX_1S
    {PWR_AWU_Prescaler_10240, 25}, // AUX_2S
    {PWR_AWU_Prescaler_10240, 50}, // AUX_4S
    {PWR_AWU_Prescaler_61440, 17}, // AUX_8S
    {PWR_AWU_Prescaler_61440, 33}, // AUX_16S
};

#define AWU_TICKS_COUNT (sizeof(awu_lut) / sizeof(awu_lut[0]))

// Объединили две функции в одну
bool awu_set_delay(awu_tick_t awu_tick)
{
    // Проверка на выход за пределы таблицы
    if (awu_tick >= AWU_TICKS_COUNT)
        return false;

    PWR_AWU_SetPrescaler(awu_lut[awu_tick].prescaler);
    PWR_AWU_SetWindowValue(awu_lut[awu_tick].window);

    return true;
}

/*********************************************************************
 * EXTI initialization
 *********************************************************************/
void EXTI_INT_INIT(void)
{
    EXTI_InitTypeDef EXTI_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    EXTI_InitStructure.EXTI_Line = EXTI_Line9;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Event;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;

    EXTI_Init(&EXTI_InitStructure);
}

/*********************************************************************
 * AWU initialization
 *********************************************************************/
void AWU_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    RCC_LSICmd(ENABLE);

    while (RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET)
        ;
}

void awu_stop(bool stop_lsi)
{
    PWR_AutoWakeUpCmd(DISABLE);
    if (stop_lsi)
    {
        RCC_LSICmd(DISABLE);
    }
}