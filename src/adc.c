/*
 * adc.c — Реализация АЦП для измерения температуры NTC
 *
 * Конфигурация ADC1:
 *   - Режим: Independent, software trigger
 *   - Регулярное: канал 3 (NTC на PD2), 57 тактов семплирования
 *   - Инжектированное: Vrefint (канал 8), 30 тактов
 *   - Автоматическая инъекция после регулярного преобразования
 *
 * Пин-конфигурация:
 *   PD2 — аналоговый вход (верхний узел делителя, сюда подключен NTC)
 *   PD3 — выход Push-Pull 50МГц, искусственная "земля" нижнего плеча
 *         делителя NTC. Делитель: Vdd -> NTC -> PD2 -> R -> PD3.
 *         PD3 = 0 ("земля") — делитель под током, на PD2 измеряем NTC.
 *         PD3 = 1 — цепь делителя разорвана сверху по логике, ток не
 *         течёт, экономия заряда батареи между измерениями.
 *
 * Формула напряжения: Vbat = (1200 * 1024) / vrefint_adc (мВ)
 * Формула температуры: бинарный поиск + линейная интерполяция по таблице
 *
 * Экономия энергии: adc_stop() отключает тактирование и переводит GPIO в Hi-Z
 */

#include "adc.h"
#include "ch32v00x.h"
#include "proteus_config.h"
#include "debug.h"


uint16_t vbat_mv = 0;  /* Напряжение питания в мВ (обновляется в adc_read) */
int16_t temperature_c = 0;
uint16_t raw_temperature_adc = 0; // сырое значение температуры с АЦП (обновляется в adc_read)

/* Таймаут циклов опроса флагов/битов АЦП (защита от зависания при сбое) */
#define ADC_WAIT_TIMEOUT 100000

/* ============================================================
 * EMA-фильтр (Exponential Moving Average)
 * ============================================================
 * Формула: avg += (raw - avg) >> shift
 * Размер: 2 байта (static uint16_t), без буфера.
 * Скорость: 1 вычитание + 1 сдвиг + 1 сложение на отсчёт.
 * Погрешность: max ошибка = 2^shift (±16 при shift=4). */
static uint16_t ema_avg;

void ema_init(uint16_t seed)
{
    ema_avg = seed;
}

uint16_t ema_update(uint16_t raw)
{
    ema_avg += (raw - ema_avg) >> EMA_SHIFT;
    return ema_avg;
}

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
 * - Настройка GPIO PD3 как искусственной земли нижнего плеча делителя (AIN)
 * - Включение режима автоматической инъекции (Regular + Injected)
 * - Регулярное преобразование: канал 3 (NTC), 57 тактов АЦП
 * - Инжектированное преобразование: Vrefint (канал 8), 30 тактов АЦП
 * - Калибровка ADC перед началом измерений
 *
 * @return  none
 */
void adc_init(void)
{
    // 1. Включаем тактирование GPIOD и ADC1
    RCC->APB2PCENR |= (1 << 5) | (1 << 9); // GPIOD(bit5) | ADC1(bit9)

    // 2. Делим PCLK2 на 4 для ADC (биты [15:14] = 0b01)
    RCC->CFGR0 = (RCC->CFGR0 & ~(0x3 << 14)) | (0x1 << 14);

    // 3. PD2 — аналоговый вход (верхний узел делителя NTC)
    GPIOD->CFGLR &= ~(0xF << 8); // 0b0000 = Analog Input

    // 4. PD3 — выход Push-Pull 50MHz (искусственная "земля" нижнего плеча делителя)
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 12)) | (0x3 << 12);
    // Делитель: Vdd -> NTC -> PD2 -> R -> PD3. Состояние по умолчанию — PD3=1,
    // ток через делитель не течёт (экономия заряда между измерениями).
    GPIOD->BSHR = (1 << 3);  // PD3 = 1, делитель обесточен

    // 5. Сброс ADC1 через RCC (как делает SDK)
    RCC->APB2PRSTR |= (1 << 9);  // ADC1RST = 1
    RCC->APB2PRSTR &= ~(1 << 9); // ADC1RST = 0

    // 6. CTLR1: Independent mode, без сканирования, без continuous, без external trigger
    ADC1->CTLR1 = 0;

    // 7. CTLR2: DataAlign=Right, EXTSEL=7 (software trigger only), ContinuousConv=OFF
    ADC1->CTLR2 = 0x000E0000; // ADC_ExternalTrigConv_None = EXTSEL[2:0]=111

    // 8. RSQR1: L[3:0]=0 (1 канал в регулярной последовательности)
    ADC1->RSQR1 = 0;

    // 9. RSQR3: SQ1 = канал 3 (NTC на PD2)
    ADC1->RSQR3 = 3;

    // 10. SAMPTR2: время семплирования регулярного канала SQ1=CH3(NTC)
    //     Коды SMP[2:0] -> циклы АЦП: 0=3, 1=9, 2=15, 3=30, 4=43, 5=57, 6=73, 7=241
    //     SMP3 [11:9]   = 5 -> 57 циклов для NTC (внешний источник, нужна большая ёмкость заряда)
    //     SMP8 [26:24]  = 3 -> 30 циклов для Vrefint (внутренний источник, заряд быстрый)
    ADC1->SAMPTR2 = (5 << 9) | (3 << 24);

    // 11. ISQR: JL=0 (1 инжектированный канал), JSQ4 = 8 (Vrefint на CH32V003)
    ADC1->ISQR = 8 << 15;

    // 12. SAMPTR1: время семплирования инжектированного канала (Rank1 = JSQ4)
    //     SDK пишет код времени семплирования сюда независимо от номера канала
    ADC1->SAMPTR1 = 3; // код 3 -> 30 циклов АЦП для Vrefint

    // 13. Калибровка ADC: CALVOLSELECT=50% (бит 25 в CTLR1)
    ADC1->CTLR1 |= (1 << 25);

    // 14. JAUTO: автоматическая инъекция после регулярного (бит 10 в CTLR1)
    ADC1->CTLR1 |= (1 << 10);

    // 15. EXTTRIG: software trigger (бит 20 в CTLR2)
    ADC1->CTLR2 |= (1 << 20);

    // 16. Включаем ADC (ADON = бит 0 в CTLR2)
    ADC1->CTLR2 |= (1 << 0);

    // 17. Задержка после включения ADC
    for (volatile int i = 0; i < 1000; i++)
        ;

    // 18. Сброс калибровки (RSTCAL = бит 3 в CTLR2)
    ADC1->CTLR2 |= (1 << 3);
    {
        uint32_t timeout = ADC_WAIT_TIMEOUT;
        while ((ADC1->CTLR2 & (1 << 3)) && --timeout)
            ;
    }

    // 19. Калибровка ADC (CAL = бит 2 в CTLR2)
    ADC1->CTLR2 |= (1 << 2);
    {
        uint32_t timeout = ADC_WAIT_TIMEOUT;
        while ((ADC1->CTLR2 & (1 << 2)) && --timeout)
            ;
    }

    // 20. Инициализация EMA-фильтра: (1200*1024)/3800 = 323 при Vdd=3.8V
    ema_init(323);
}

void adc_read(void)
{
    // Включаем делитель напряжения: PD3 = 0 ("земля" нижнего плеча, ток через NTC течёт)
    GPIOD->BCR = (1 << 3);

    // Задержка после подключения делителя.
    for (volatile int i = 0; i < 170; i++)
        ;

    // Сбрасываем только флаги EOC (бит1) и JEOC (бит2), запись 0 в STATR сбрасывает бит.
    // Остальные биты регистра не трогаем (записываем 1 = "оставить как есть").
    ADC1->STATR &= ~((1 << 1) | (1 << 2));

    // Запуск регулярного преобразования (EXTTRIG + SWSTART = биты 20+22 в CTLR2)
    ADC1->CTLR2 |= 0x00500000;

    // Ожидаем завершения регулярного преобразования (EOC, бит 1), с таймаутом
    uint32_t timeout = ADC_WAIT_TIMEOUT;
    while (!(ADC1->STATR & (1 << 1)) && --timeout)
        ;

    // Ожидаем завершения инжектированного преобразования (JEOC, бит 2), с таймаутом
    timeout = ADC_WAIT_TIMEOUT;
    while (!(ADC1->STATR & (1 << 2)) && --timeout)
        ;

    // Читаем результат регулярного преобразования (NTC).
    // Если случился таймаут выше, RDATAR/IDATAR1 могут содержать значение
    // от предыдущего цикла измерения — это не идеально, но не зависание.
    raw_temperature_adc = ADC1->RDATAR;

    // Читаем результат инжектированного преобразования (Vrefint)
    uint16_t vrefint_adc = ema_update(ADC1->IDATAR1);

    // Отключаем делитель напряжения: PD3 = 1 (ток через делитель не течёт, экономия заряда)
    GPIOD->BSHR = (1 << 3);

    // Вычисляем напряжение питания (мВ): Vrefint=1.2V, 10-бит ADC
    vbat_mv = (1200 * 1024) / vrefint_adc;

    // Расчет температуры по таблице
    temperature_c = calc_temperature(raw_temperature_adc);

#ifdef PRINT_ADC
    // Выводим результат
    EE_PRINTF("%04d, %04d -> Vbat: %ld mV | Temp: %d.%d C\r\n",
              vrefint_adc, raw_temperature_adc, vbat_mv, temperature_c / 10, temperature_c % 10);
#endif
}

/**
 * @brief Полная остановка АЦП для экономии энергии
 *
 * Выполняет:
 *   1. Сброс флага ADON (бит 0 CTLR2) — отключение АЦП
 *   2. Отключение тактирования АЦП (бит 9 APB2PCENR = RCC_ADC1EN)
 *   3. Отключение GPIO аналоговых входов (PD2, PD3 в Hi-Z)
 *
 * После вызова АЦП полностью обесточен. Для повторного использования
 * вызвать adc_init() (требуется повторная калибровка).
 */
void adc_stop(void)
{
    /* Если АЦП уже отключено (тактирование выключено или ADON=0) — выходим */
    if (!(RCC->APB2PCENR & RCC_ADC1EN) || !(ADC1->CTLR2 & (1 << 0)))
        return;

    /* 1. Отключаем АЦП (ADON = 0) */
    ADC1->CTLR2 &= ~(1 << 0);

    /* 2. Отключаем тактирование ADC1 (бит 9 APB2PCENR) */
    RCC->APB2PCENR &= ~RCC_ADC1EN;

    /* 3. PD2 и PD3 переводим в Analog Input — высокое импедансное состояние (Hi-Z)
     *    PD3 (искусственная "земля" нижнего плеча делителя) — Hi-Z, делитель обесточен
     *    PD2 (верхний узел делителя, вход NTC) — Hi-Z, нет утечки через АЦП */
    GPIOD->CFGLR &= ~(0xF << 8);   /* PD2: 0b0000 = Analog Input (Hi-Z) */
    GPIOD->CFGLR &= ~(0xF << 12);  /* PD3: 0b0000 = Analog Input (Hi-Z) */
}

uint8_t bat_get_flashes(void) {
	static const uint16_t thresholds[] = {3000, 3200, 3400, 3600, 3800, 4000};
	for (uint8_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
		if (vbat_mv < thresholds[i]) return i;
	}
	return 6;
}
