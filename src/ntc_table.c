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
    // 1. Включаем тактирование GPIOD и ADC1
    RCC->APB2PCENR |= (1 << 5) | (1 << 9);  // GPIOD(bit5) | ADC1(bit9)

    // 2. Делим PCLK2 на 4 для ADC (биты [15:14] = 0b01)
    RCC->CFGR0 = (RCC->CFGR0 & ~(0x3 << 14)) | (0x1 << 14);

    // 3. PD2 — аналоговый вход (NTC)
    GPIOD->CFGLR &= ~(0xF << 8);  // 0b0000 = Analog Input

    // 4. PD3 — выход Push-Pull 50MHz (искусственная земля)
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 12)) | (0x3 << 12);
    GPIOD->BCR = (1 << 3);  // PD3 = 0

    // 5. Сброс ADC1 через RCC (как делает SDK)
    RCC->APB2PRSTR |= (1 << 9);   // ADC1RST = 1
    RCC->APB2PRSTR &= ~(1 << 9);  // ADC1RST = 0

    // 6. CTLR1: Independent mode, без сканирования, без continuous, без external trigger
    ADC1->CTLR1 = 0;

    // 7. CTLR2: DataAlign=Right, EXTSEL=7 (software trigger only), ContinuousConv=OFF
    ADC1->CTLR2 = 0x000E0000;  // ADC_ExternalTrigConv_None = EXTSEL[2:0]=111

    // 8. RSQR1: L[3:0]=0 (1 канал в регулярной последовательности)
    ADC1->RSQR1 = 0;

    // 9. RSQR3: SQ1 = канал 3 (NTC на PD2)
    ADC1->RSQR3 = 3;

    // 10. SAMPTR2: SMP3 [11:9] = 5 (57 тактов) для NTC
    //     SMP8 [26:24] = 3 (30 тактов) для Vrefint (внутренний, без паразитной ёмкости)
    ADC1->SAMPTR2 = (5 << 9) | (3 << 24);

    // 11. ISQR: JL=0 (1 инжектированный канал), JSQ4 = 8 (Vrefint на CH32V003)
    ADC1->ISQR = 8 << 15;

    // 12. SAMPTR1: время семплирования для injected каналов (SDK пишет сюда для Rank 1)
    ADC1->SAMPTR1 = 3;  // 30 тактов для Vrefint

    // 12. Калибровка ADC: CALVOLSELECT=50% (бит 25 в CTLR1)
    ADC1->CTLR1 |= (1 << 25);

    // 13. JAUTO: автоматическая инъекция после регулярного (бит 10 в CTLR1)
    ADC1->CTLR1 |= (1 << 10);

    // 14. EXTTRIG: software trigger (бит 20 в CTLR2)
    ADC1->CTLR2 |= (1 << 20);

    // 15. Включаем ADC (ADON = бит 0 в CTLR2)
    ADC1->CTLR2 |= (1 << 0);

    // 16. Задержка после включения ADC
    for (volatile int i = 0; i < 1000; i++);

    // 17. Сброс калибровки (RSTCAL = бит 3 в CTLR2)
    ADC1->CTLR2 |= (1 << 3);
    while (ADC1->CTLR2 & (1 << 3));

    // 18. Калибровка ADC (CAL = бит 2 в CTLR2)
    ADC1->CTLR2 |= (1 << 2);
    while (ADC1->CTLR2 & (1 << 2));
}

void ntc_read(void)
{
    // Сбрасываем флаги EOC и JEOC (write-0-to-clear)
    ADC1->STATR = ~(0x02 | 0x04);

    // Запуск регулярного преобразования (EXTTRIG + SWSTART = биты 20+22 в CTLR2)
    ADC1->CTLR2 |= 0x00500000;

    // Ожидаем завершения регулярного преобразования (EOC, бит 1)
    while (!(ADC1->STATR & (1 << 1)));

    // Ожидаем завершения инжектированного преобразования (JEOC, бит 2)
    while (!(ADC1->STATR & (1 << 2)));

    // Читаем результат регулярного преобразования (NTC)
    uint16_t raw_temperature_adc = ADC1->RDATAR;

    // Читаем результат инжектированного преобразования (Vrefint)
    uint16_t vrefint_adc = ADC1->IDATAR1;

    // Вычисляем напряжение питания (мВ): Vrefint=1.2V, 10-бит ADC
    uint32_t vbat_mv = (1200 * 1024) / vrefint_adc;

    // Расчет температуры по таблице
    int16_t temperature = calc_temperature(raw_temperature_adc);

    // Выводим результат
    EE_PRINTF("%04d, %04d -> Vbat: %ld mV | Temp: %d.%d C\r\n",
              vrefint_adc, raw_temperature_adc, vbat_mv, temperature / 10, temperature % 10);

}
