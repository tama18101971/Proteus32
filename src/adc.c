/**
 * @file adc.c
 * @brief ADC driver for NTC temperature and Vbat measurement
 *
 * ADC1 configuration:
 *   - Mode: Independent, software trigger
 *   - Regular: channel 3 (NTC on PD2), 57 sample clocks
 *   - Injected: Vrefint (channel 8), 30 sample clocks
 *   - Auto-injection after regular conversion
 *
 * Pin config:
 *   PD2 — analog input (NTC divider top node)
 *   PD3 — output PP 50 MHz, artificial "GND" for divider bottom leg
 *         Divider: Vdd -> NTC -> PD2 -> R -> PD3.
 *         PD3 = 0 (GND) — divider active, NTC measured on PD2
 *         PD3 = 1 — divider chain broken, no current draw
 *
 * Voltage: Vbat = (1200 * 1024) / vrefint_adc (mV)
 * Temperature: binary search + linear interpolation in termo_table
 *
 * Power saving: adc_stop() disables clock and sets GPIO to Hi-Z
 *

 * Ported from Proteus84 (AVR) — scan_voltage() is the equivalent of
 * AVR adc.c:scan_voltage() with limiter[2][6] and step-down logic.
 */

#include "adc.h"
#include "ch32v00x.h"
#include "proteus_config.h"
#include "debug.h"
#include "ui.h"
#include "app_data.h"
#include "ob.h"
#include "aux_leds.h"
#include "dac.h"
#include "timers.h"
#include "awu.h"
#include "led_gpio.h"


uint16_t vbat_mv = 0;  /* Напряжение питания в мВ (обновляется в adc_read) */
int16_t temperature_c = 0;
uint16_t raw_temperature_adc = 0; // сырое значение температуры с АЦП (обновляется в adc_read)

uint8_t volt_max_mode = CMODE_TURBO;
uint8_t volt_stop_mode = CMODE_TURBO;

/* Счётчик ступеней снижения яркости UI1 при просадке АКБ.
 * Сбрасывается в 5 в on_driver() (аналог AVR button.c:209).
 * Глобальный — чтобы on_driver() в ui.c мог его сбросить. */
uint8_t down_count = 5;

/* Храповик потолка яркости UI1 (ramping): монотонно убывает по мере
 * просадки АКБ и сбрасывается лишь на новой попытке включения с
 * подтверждённо восстановившимся напряжением (reset_max_step()). */
uint16_t volt_step_ratchet = MAX_STEP;

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
static bool ema_seeded;

/** @brief Initialise EMA filter with seed value. */
void ema_init(uint16_t seed)
{
    ema_avg = seed;
    ema_seeded = false;
}

/** @brief Update EMA filter with new raw sample. */
uint16_t ema_update(uint16_t raw)
{
    if (!ema_seeded)
    {
        ema_avg = raw;
        ema_seeded = true;
        return raw;
    }

    int16_t diff = (int16_t)raw - (int16_t)ema_avg;
    ema_avg += diff >> EMA_SHIFT;
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

/** @brief Calculate temperature from ADC value using lookup + linear interpolation. */
int16_t calc_temperature(uint16_t adcValue)
{
    uint8_t l = 0;
    uint8_t r = TABLE_SIZE - 1;

    if (adcValue <= termo_table[0])
        return TEMPERATURE_TABLE_START;

    if (adcValue >= termo_table[r])
        return TEMPERATURE_TABLE_START + (TABLE_SIZE - 1) * TEMPERATURE_TABLE_STEP;

    uint8_t m;
    while ((r - l) > 1)
    {
        m = (l + r) >> 1;
        if (adcValue < termo_table[m])
            r = m;
        else
            l = m;
    }

    uint16_t vl = termo_table[l];
    uint16_t vr = termo_table[r];

    int16_t temp = TEMPERATURE_TABLE_START + l * TEMPERATURE_TABLE_STEP;

    if (vr != vl)
    {
        uint32_t diff_adc = adcValue - vl;
        uint32_t diff_table = vr - vl;
        temp += (int16_t)((diff_adc * TEMPERATURE_TABLE_STEP + (diff_table >> 1)) / diff_table);
    }
    return temp;
}

/**
 * @brief Apply tc calibration offset to raw temperature ADC.
 * @return Corrected ADC value (clamped to ADC_MIN_VAL..ADC_MAX_VAL).
 */
uint16_t get_exact_temp_adc(void)
{
    int16_t v = (int16_t)raw_temperature_adc - (int16_t)ob_GetTc();
    if (v < ADC_MIN_VAL) v = ADC_MIN_VAL;
    if (v > ADC_MAX_VAL) v = ADC_MAX_VAL;
    return (uint16_t)v;
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

    /* First valid Vrefint conversion seeds EMA exactly; a guessed nominal
     * voltage could briefly conceal a flat cell after wake. */
    ema_init(0);
}

/**
 * @brief Perform ADC read: NTC temperature + Vrefint -> Vbat.
 *
 * Powers the NTC divider (PD3=0), waits for settling, starts regular+injected
 * conversion, reads results, then powers down the divider (PD3=1).
 * Updates raw_temperature_adc, vbat_mv, temperature_c.
 */
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
    uint16_t vrefint_raw = ADC1->IDATAR1;

    // Отключаем делитель напряжения: PD3 = 1 (ток через делитель не течёт, экономия заряда)
    GPIOD->BSHR = (1 << 3);

    /* A zero Vref result means that conversion did not provide a usable
     * sample.  Keep the previous voltage rather than dividing by zero or
     * teaching the EMA an impossible value. */
    if (vrefint_raw == 0u)
        return;

    uint16_t vrefint_adc = ema_update(vrefint_raw);

    // Вычисляем напряжение питания (мВ): Vrefint=1.2V, 10-бит ADC
    vbat_mv = (1200 * 1024) / vrefint_adc;

    // Применяем калибровку vc из Option Bytes (int8_t, ±127 мВ)
    vbat_mv += (int16_t)ob_GetVc();

    // Расчет температуры по таблице (с коррекцией tc)
    temperature_c = calc_temperature(get_exact_temp_adc());

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

/* ============ Защита по разряду батареи ============ */

/* Пороги напряжения для 1S Li-ion (мВ):
 *   [0] = критический разряд → принудительный сон
 *   [1] = только CMODE_MOON
 *   [2] = CMODE_MIN
 *   [3] = CMODE_MID0
 *   [4] = CMODE_MID
 *   [5] = восстановление / без ограничений (CMODE_TURBO)
 */
static const uint16_t mode_swt[6] = {
    2700, 2750, 2800, 2900, 3000, 3100
};

/**
 * @brief Вычисляет максимально допустимый режим UI2 по текущему напряжению.
 * @param exact_v Точное напряжение питания в мВ.
 * @return Индекс максимального режима (CMODE_MOON..CMODE_TURBO).
 */
static uint8_t calc_volt_max_mode(uint16_t exact_v)
{
    for (uint8_t i = CMODE_MOON; i < CMODE_TURBO; i++)
        if (exact_v < (mode_swt[i] + 1)) return i;
    return CMODE_TURBO;
}

/* ============ Потолок яркости UI1 по напряжению (ramping) ============
 * Аналог AVR adc.c limiter[2][6] + ratchet_max_step().
 * volt_max_tbl — пороги в мВ (по возрастанию), step_max_tbl —
 * соответствующие 9-битные шаги (0..511), масштабированные из AVR 8-бит.
 * volt_step_ratchet монотонно убывает (MIN) до явного безопасного восстановления. */
static const uint16_t volt_max_tbl[6] = {2500, 2820, 2930, 3080, 3190, 3300};
static const uint16_t step_max_tbl[6] = {120, 240, 310, 400, 463, 511};

/** @brief Чистый расчёт потолка по текущему vbat_mv (без побочных эффектов). */
static uint16_t calc_step_ceiling(void)
{
    uint8_t i = 6;
    while (i > 0)
    {
        i--;
        if (vbat_mv > volt_max_tbl[i]) break;
    }
    return step_max_tbl[i];
}

/** @brief Применить храповик: понизить volt_step_ratchet и вернуть его. */
uint16_t ratchet_max_step(void)
{
    uint16_t s = calc_step_ceiling();
    if (s < volt_step_ratchet) volt_step_ratchet = s;
    return volt_step_ratchet;
}

/**
 * @brief Сброс храповика потолка яркости UI1.
 *
 * Сбрасывает монотонно убывающий потолок (volt_step_ratchet) на MAX_STEP.
 * Вызывается при новой попытке включения с подтверждённо восстановившимся
 * напряжением, чтобы снять ограничение, наложленное просадкой АКБ.
 */
void reset_max_step(void)
{
    volt_step_ratchet = MAX_STEP;
}

extern uint8_t need_sleep;

/**
 * @brief Необратимое выключение: обесточить нагрузку и уйти в вечный STANDBY.
 *
 * Защита от глубокого разряда. Обесточивает нагрузку (LED через off_red/off_grn,
 * DAC PD0), переводит I2C и делитель NTC в Hi-Z, отключает AWU/LSI и все
 * источники пробуждения (кнопка EXTI3, AWU EXTI9 и прочие EXTI-линии).
 * Вечный цикл STANDBY: функция не возвращается (в отличие от AWU_Sleep_Now(),
 * который мог вернуться и отменить PDDS).
 */
static void down_forever(void)
{
    /* 1. Обесточить нагрузку: AUX-светодиоды погасить через штатные ф-ции
     *    (уважают особенность PC6/PC7), питание DAC (PD0)=0. */
    off_red();
    off_grn();
    DAC_PWR_PORT->BCR = DAC_PWR_PIN;

    /* 2. I2C-линии PC1/PC2 и делитель NTC PD2/PD3 → Hi-Z: нет утечек.
     *    Тактирование АЦП/TIM2 выключать не нужно — в STANDBY чип и так
     *    полностью обесточен (PDDS=1). */
    GPIOC->CFGLR &= ~((0x0F << 4) | (0x0F << 8));
    GPIOD->CFGLR &= ~((0xF << 8) | (0xF << 12));

    /* 3. Остановить AWU и LSI — не потребляют ток в STANDBY и не дают
     *    события пробуждения (EXTI Line9). */
    PWR->AWUCSR &= ~PWR_AWUCSR_AWUEN;
    RCC->RSTSCKR &= ~RCC_LSION;

    /* 4. Замаскировать ВСЕ EXTI-прерывания и события: ни кнопка (Line3),
     *    ни AWU (Line9) не смогут разбудить MCU из STANDBY. Очистка
     *    триггеров и pending-флагов — чтобы нажатие во время входа
     *    в сон не сформировало отложенное событие. */
    EXTI->INTENR = 0;
    EXTI->EVENR  = 0;
    EXTI->RTENR  = 0;
    EXTI->FTENR  = 0;
    EXTI->INTFR  = 0;

    /* 5. Снять IRQ EXTI7_0 (кнопка), AWU и TIM2 из NVIC — страховка на
     *    случай теоретического пробуждения: обработчики не выполнятся. */
    NVIC->IRER[0] = (1 << 20) | (1 << 21);
    NVIC->IRER[1] = (1 << (38 & 0x1F));

    /* 6. Вечный STANDBY: AWU_Sleep_Now() содержит тяжёлую последовательность
     *    __WFI() (WCH: PDDS+SLEEPDEEP+WFE) — она уже слинкована и используется
     *    main loop, поэтому здесь её не инлайним, а вызываем. Если каким-то
     *    образом WFI вернулся (все EXTI замаскированы — вернуться не может),
     *    цикл снова уходит в STANDBY. Управление не возвращается. */
    for (;;)
    {
        AWU_Sleep_Now();
    }
}

/**
 * @brief Battery undervoltage protection (call at 30 Hz).
 *
 * For UI1 (ramping): step-down brightness at each threshold, hard cutoff at 2700 mV.
 * For UI2 (discrete): reduce mode automatically, hard cutoff if already at mode 0.
 * Monotonic: volt_stop_mode only decreases, recovers on reboot.
 */
void scan_voltage(void)
{
    static uint8_t voff_count = 0;
    static uint8_t voff_count_off = 0;
    uint8_t ui_index = EE.uif.ui_index;

    volt_max_mode = calc_volt_max_mode(vbat_mv);

    if (driver_mode < MODE_ON) return;

    /* Ramping UI (brightness control UI1) */
    if (ui_index == 1)
    {
        /* Critical undervoltage: below mode_swt[0] */
        if (vbat_mv < mode_swt[0])
        {
            if (++voff_count_off > 3)
            {
                voff_count_off = 0;
                down_forever();
                return;
            }
        }
        else
        {
            voff_count_off = 0;

            /* Step-down brightness on voltage sag */
            if (vbat_mv < mode_swt[down_count])
            {
                if (++voff_count > 3)
                {
                    voff_count = 0;
                    if (down_count)
                    {
                        down_count--;
                        step_user = step_user - (step_user >> 2);
                        set_current(step_user, SS_SLOW);
                    }
                }
            }
            else
            {
                voff_count = 0;

                /* Recovery: voltage above top threshold */
                if (vbat_mv > mode_swt[5])
                    down_count = 5;
            }
        }
    }

    /* Discrete UI (mode-based UI2) */
    if ((ui_index > 1) && (ui_index < 4))
    {
        if (mode_current && (vbat_mv < mode_swt[mode_current - 1]))
        {
            if (++voff_count > 3)
            {
                voff_count = 0;
                if (volt_stop_mode > (--mode_current))
                    volt_stop_mode = mode_current;
                if (mode_current)
                {
                    if (driver_mode != MODE_STROB)
                        set_current(get_stepi(mode_current), SS_SLOW);
                }
                else
                {
                    down_forever();
                    return;
                }
            }
        }
        else
        {
            voff_count = 0;
        }
    }
}

/** @brief Get battery indicator flash count (0-6) based on Vbat. */
uint8_t bat_get_flashes(void) {
	static const uint16_t thresholds[] = {3000, 3200, 3400, 3600, 3800, 4000};
	for (uint8_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
		if (vbat_mv < thresholds[i]) return i;
	}
	return 6;
}
