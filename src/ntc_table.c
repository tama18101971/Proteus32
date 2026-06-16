#include "ntc_table.h"
#include "ch32v00x.h"
#include "ch32v00x_adc.h"
#include "ch32v00x_rcc.h"
#include "ch32v00x_gpio.h"
#include "debug.h"
#include "config.h"

// Макрос расчета напряжения питания (Vdd) в милливольтах на основе кода Vrefint (встроенный источник 1.2В)
#define __LL_ADC_CALC_VREFANALOG_VOLTAGE(__VREFINT_ADC_DATA__) \
    (1200 * 1024 / (__VREFINT_ADC_DATA__))

/* Таблица ADC-кодов для NCP18XH103F03RB (схема D с Rpar=77.3k) */
/* Диапазон: -20...+80°C, шаг 1°C, значения возрастают при нагреве */
static const uint16_t termo_table[] = {
    130, 143, 157, 170, 184, 197, 211, 224, 238, 251,
    265, 272, 279, 286, 293, 300, 307, 314, 321, 328,
    335, 342, 350, 357, 365, 372, 380, 387, 395, 402,
    410, 419, 428, 436, 445, 454, 463, 471, 480, 489,
    498, 507, 515, 524, 533, 542, 550, 559, 567, 576,
    585, 592, 599, 606, 613, 620, 627, 634, 641, 647,
    654, 661, 668, 674, 681, 688, 695, 702, 708, 715,
    722, 728, 733, 739, 745, 750, 756, 762, 767, 773,
    779, 784, 788, 793, 797, 802, 807, 811, 816, 820,
    825, 830, 835, 840, 846, 851, 856, 861, 866, 871,
    876};

#define TABLE_SIZE (sizeof(termo_table) / sizeof(termo_table[0]))

/* Вычисляет температуру по ADC-коду с линейной интерполяцией */
int16_t calc_temperature(uint16_t adcValue)
{
    uint8_t l = 0;
    uint8_t r = TABLE_SIZE - 1;

    /* Если код ниже минимального — возвращаем нижнюю границу диапазона */
    if (adcValue <= termo_table[0])
        return TEMPERATURE_TABLE_START;

    /* Если код выше максимального — возвращаем верхнюю границу диапазона */
    if (adcValue >= termo_table[r])
        return TEMPERATURE_TABLE_START + (TABLE_SIZE - 1) * TEMPERATURE_TABLE_STEP;

    /* Бинарный поиск для нахождения интервала [l, r] */
    while ((r - l) > 1)
    {
        uint8_t m = (l + r) >> 1;
        if (adcValue < termo_table[m])
            r = m;
        else
            l = m;
    }

    /* Получаем границы интервала */
    uint16_t vl = termo_table[l];
    uint16_t vr = termo_table[r];

    /* Базовая температура начала интервала */
    int16_t temp = TEMPERATURE_TABLE_START + l * TEMPERATURE_TABLE_STEP;

    /* Линейная интерполяция внутри интервала */
    if (vr != vl)
    {
        uint32_t diff_adc = adcValue - vl;
        uint32_t diff_table = vr - vl;
        temp += (int16_t)((diff_adc * TEMPERATURE_TABLE_STEP + (diff_table >> 1)) / diff_table);
    }
    return temp;
}

/*********************************************************************
 * @fn      ADC_Function_Init
 *
 * @brief   Инициализирует ADC для измерения температуры NTC.
 *
 * @details
 * - Настройка GPIO PD2 как аналогового входа (NTC)
 * - Настройка GPIO PD3 как искусственной земли (AIN)
 * - Включение режима автоматической инъекции (Regular + Injected)
 * - Регулярное преобразование: канал 3 (NTC), 241 такт
 * - Инжектированное преобразование: Vrefint, 241 такт
 * - Калибровка ADC перед началом измерений
 *
 * @return  none
 */
void ntc_adc_Init(void)
{
    ADC_InitTypeDef ADC_InitStructure = {0};
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    // Включаем периферию: GPIOD и ADC1
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1, ENABLE);

    // Делим системную частоту на 8 для ADC (стандартная практика)
    RCC_ADCCLKConfig(RCC_PCLK2_Div8);

    // Настраиваем PD2 как аналоговый вход (NTC терморезистор)
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOD, &GPIO_InitStructure);

    // Настраиваем PD3 как искусственную землю
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOD, &GPIO_InitStructure);
    GPIO_ResetBits(GPIOD, GPIO_Pin_3);

    // Сбрасываем настройки ADC1 к заводским значениям
    ADC_DeInit(ADC1);

    // Настройка режима работы ADC
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;                  // Независимый режим (один канал)
    ADC_InitStructure.ADC_ScanConvMode = DISABLE;                       // Отключаем сканирование каналов
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;                 // Однократное преобразование
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None; // Внутренний триггер
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;              // Правое выравнивание данных (стандарт)
    ADC_InitStructure.ADC_NbrOfChannel = 1;                             // Один канал в регулярной группе

    ADC_Init(ADC1, &ADC_InitStructure);

    // Настройка инъектированной последовательности (Vrefint)
    ADC_InjectedSequencerLengthConfig(ADC1, 1);

    // Конфигурация регулярного канала (NTC на PD2)
    ADC_RegularChannelConfig(ADC1, ADC_Channel_3, 1, ADC_SampleTime_241Cycles);

    // Конфигурация инъектированного канала (Vrefint для расчета Vdd)
    ADC_InjectedChannelConfig(ADC1, ADC_Channel_Vrefint, 1, ADC_SampleTime_241Cycles);

    // Калибровка ADC (50% отклонение - стандартная настройка CH32)
    ADC_Calibration_Vol(ADC1, ADC_CALVOL_50PERCENT);

    // Включение автоматической инъекции после каждого регулярного преобразования
    ADC_AutoInjectedConvCmd(ADC1, ENABLE);

    // Включение ADC
    ADC_Cmd(ADC1, ENABLE);

    // Калибровка ADC (обязательная процедура)
    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1))
        ;
    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1))
        ;
}

void ntc_read(void)
{
    // ВАЖНО: Сбрасываем флаги перед стартом, чтобы не считать старые данные
    ADC_ClearFlag(ADC1, ADC_FLAG_EOC | ADC_FLAG_JEOC);

    // Запуск программного преобразования ADC (регулярный канал - NTC)
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);

    // Ожидание завершения обоих преобразований:
    // 1. Регулярное (NTC) - флаг EOC
    // 2. Инжектированное (Vrefint) - флаг JEOC
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC))
        ;
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_JEOC))
        ;

    // Читаем чистые физические коды из регистров результатов:
    // raw_temperature_adc - результат преобразования NTC (регулярный канал)
    uint16_t raw_temperature_adc = ADC_GetConversionValue(ADC1);

    // vrefint_adc - результат преобразования внутреннего опорного напряжения (инжектированный канал)
    uint16_t vrefint_adc = ADC_GetInjectedConversionValue(ADC1, ADC_InjectedChannel_1);

    // Вычисляем точное напряжение питания (для 10 бит 1024 - верно)
    // Формула: Vdd = Vref * (2^N / ADC_Vrefint_code)
    uint32_t vbat_mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(vrefint_adc);

    // Расчет температуры по таблице NCP18XH103F03RB с линейной интерполяцией
    int16_t temperature = calc_temperature(raw_temperature_adc);

    // Выводим честный результат в терминал:
    // Формат: Vrefint_code, NTC_code -> Vbat: X mV | Temp: Y.Z C
    EE_PRINTF("[NTC] %04d, %04d -> Vbat: %ld mV | Temp: %d.%d C\r\n",
           vrefint_adc, raw_temperature_adc, vbat_mv, temperature / 10, temperature % 10);
}
