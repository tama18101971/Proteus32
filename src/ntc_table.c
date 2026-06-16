#include "ntc_table.h"

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
    876
};

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
    while ((r - l) > 1) {
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
    if (vr != vl) {
        uint32_t diff_adc = adcValue - vl;
        uint32_t diff_table = vr - vl;
        temp += (int16_t)((diff_adc * TEMPERATURE_TABLE_STEP + (diff_table >> 1)) / diff_table);
    }
    return temp;
}
