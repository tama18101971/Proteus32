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
    RCC->APB2PCENR |= (1 << 5) | (1 << 9);  // RCC_APB2Periph_GPIOD(bit5) | RCC_APB2Periph_ADC1(bit9)

    // 2. Делим PCLK2 на 8 для ADC (биты [15:14] в CFGR0 = 0b11)
    RCC->CFGR0 = (RCC->CFGR0 & ~(0x3 << 14)) | (0x3 << 14);

    // 3. PD2 — аналоговый вход (NTC терморезистор)
    GPIOD->CFGLR &= ~(0xF << 8);  // 0b0000 = Analog Input

    // 4. PD3 — выход Push-Pull 50MHz (искусственная земля)
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 12)) | (0x3 << 12);
    GPIOD->BCR = (1 << 3);  // PD3 = 0

    // 5. Настраиваем CTLR1:
    //    - Independent mode (биты [19:16] = 0)
    //    - Scan mode OFF (бит 8 = 0)
    //    - Continuous mode OFF (бит 1 = 0)
    //    - Auto-injected ON (бит 10 = 1)
    //    - Calibration voltage 50% (биты [26:25] = 0b01)
    ADC1->CTLR1 = 0;
    ADC1->CTLR1 |= (1 << 10);   // JAUTO
    ADC1->CTLR1 |= (1 << 25);   // CALVOLSELECT = 50% (bit 25, НЕ bit 26)

    // 6. CTLR2: внешний триггер выключен, выравнивание вправо
    ADC1->CTLR2 = 0;

    // 7. Регулярная последовательность: 1 канал
    ADC1->RSQR1 = 0;

    // 8. Канал 3 (NTC) на позиции SQ1
    ADC1->RSQR3 = 3;

    // 9. Время семплирования канала 3 (NTC) в SAMPTR2
    ADC1->SAMPTR2 = (0x7 << 9);  // SMP3 [11:9] = 0b111 (241 такт)

    // 10. Время семплирования Vrefint в SAMPTR1 (SDK пишет injected в SAMPTR1)
    ADC1->SAMPTR1 = 0x7;  // SMP0 [2:0] = 0b111 (241 такт)

    // 11. Инжектированная последовательность: 1 канал, JSQ1 = канал 0 (Vrefint)
    ADC1->ISQR = 0;  // JL=0 (1 канал), JSQ1=0 (Vrefint)

    // 12. Включаем ADC (ADON = 1)
    ADC1->CTLR2 |= (1 << 0);

    // 13. Задержка после включения ADC (требуется даташитом)
    for (volatile int i = 0; i < 1000; i++);

    // 14. Сброс калибровки (только после ADON!)
    ADC1->CTLR2 |= (1 << 3);   // RSTCAL = 1
    while (ADC1->CTLR2 & (1 << 3));

    // 15. Калибровка ADC
    ADC1->CTLR2 |= (1 << 2);   // CAL = 1
    while (ADC1->CTLR2 & (1 << 2));
}

void ntc_read(void)
{
    // Сбрасываем флаги EOC и JEOC
    ADC1->STATR = ~(0x02 | 0x04);  // write-0-to-clear: 0xFFF9

    // Убеждаемся что ADON включен (на случай сброса после калибровки)
    ADC1->CTLR2 |= (1 << 0);

    // Запуск регулярного преобразования (SWSTART = 1)
    ADC1->CTLR2 |= (1 << 22);

    // Ожидаем завершения регулярного преобразования (EOC)
    while (!(ADC1->STATR & (1 << 1)));

    // Ожидаем завершения инжектированного преобразования (JEOC)
    while (!(ADC1->STATR & (1 << 2)));

    // Читаем результат регулярного преобразования (NTC)
    uint16_t raw_temperature_adc = ADC1->RDATAR;

    // Читаем результат инжектированного преобразования (Vrefint)
    uint16_t vrefint_adc = ADC1->IDATAR1;

    // Вычисляем напряжение питания (мВ)
    uint32_t vbat_mv = (1200 * 1024) / vrefint_adc;

    // Расчет температуры по таблице
    int16_t temperature = calc_temperature(raw_temperature_adc);

    // Выводим результат
    EE_PRINTF("%04d, %04d -> Vbat: %ld mV | Temp: %d.%d C\r\n",
              vrefint_adc, raw_temperature_adc, vbat_mv, temperature / 10, temperature % 10);

    // Задержка 500 мс между измерениями
    Delay_Ms(500);
}
