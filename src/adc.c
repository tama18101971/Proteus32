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
 * Voltage: считается в кодах Vrefint, mV = (1200 * 1024) / vrefint_adc
 * вычисляется только для отладки (vbat_to_mv).
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


/* Напряжение целиком в кодах Vrefint: vbat_mv = floor(1228800/vrefint_adc).
 * Горячий путь (scan_voltage и др.) работает с vrefint_adc и кодовыми
 * порогами, без единого деления.  mB нужны только для отладочного вывода
 * (vbat_to_mv()). */
uint16_t vrefint_adc = 0;    /* код Vrefint после EMA (обновляется в adc_read) */
bool voltage_ok = false;     /* false = нет валидной свежей выборки (сбой/таймаут) */
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

#ifdef USE_EX_AUX
/** @brief Voltage thresholds for extended aux warning (Vrefint codes, ascending).
 *  ex_lvls[0] = FAST (~3.88 V), [1] = MEDIUM (~3.69 V), [2] = SLOW (~3.52 V). */
static const uint16_t ex_lvls[3] = {
    MV_LT(3880),
    MV_LT(3690),
    MV_LT(3520)
};

/**
 * @brief Extended aux voltage warning check.
 * @param eff_vcode Calibrated Vrefint code (vcode - vc).
 *
 * Compares effective voltage against three thresholds and triggers
 * progressively more urgent blink patterns via start_exaux_voltage().
 * Only active when eaux_onoff is set and driver is in MODE_ON.
 */
static void exaux(int32_t eff_vcode)
{
    if (!EE.uif.eaux_onoff) return;
    if (driver_mode != MODE_ON) return;

    if (eff_vcode >= ex_lvls[2])       vwarning = VWARNING_SLOW;
    else if (eff_vcode >= ex_lvls[1])  vwarning = VWARNING_MEDIUM;
    else if (eff_vcode >= ex_lvls[0])  vwarning = VWARNING_FAST;
    else                                vwarning = VWARNING_NONE;

    if (vwarning != vwarning_old) {
        vwarning_old = vwarning;
        if (vwarning != VWARNING_NONE)
            start_exaux_voltage((WARNING_Fr_t)vwarning);
        else
            stop_aux_if(IDM_VWARNING, false);
    }
}
#endif

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
#if defined(UART_DEBUG) || defined(PRINT_ADC)

/* Таблица ADC-кодов для NCP18XH103F03RB (схема D с Rpar=77.3k) */
/* Диапазон: -20...+80°C, шаг 1°C, значения возрастают при нагреве. */
/* Сжато до 8-битных дельт, по 2 в ниббл: соседние дельты <= 14, 100 дельт
 * укладываются в 50 байт вместо 101 x uint16 = 202 B.  Потребитель —
 * только отладочный вывод temperature_c; защита/термо-лимитер работают
 * в кодах АЦП и эту таблицу не используют. */
#define TERMO_BASE_ADC    130u  /* код АЦП на -20°C (бывший termo_table[0]) */
#define TERMO_DELTA_COUNT 100u

static const uint8_t termo_table[] = {
    0xED, 0xED, 0xED, 0xED, 0xED,   /* 13,14 x5 */
    0x77, 0x77, 0x77, 0x77, 0x77,   /*  7, 7 x5 */
    0x87, 0x87, 0x87, 0x87, 0x87,   /*  7, 8 x5 */
    0x99, 0x98, 0x99, 0x98, 0x99,   /* 9,9 / 8,9 чередование */
    0x89, 0x99, 0x89, 0x89, 0x99,
    0x77, 0x77, 0x77, 0x77, 0x76,
    0x77, 0x76, 0x77, 0x77, 0x76,
    0x67, 0x65, 0x56, 0x66, 0x65,
    0x56, 0x54, 0x45, 0x55, 0x54,
    0x55, 0x55, 0x56, 0x55, 0x55,
};

/**
 * @brief Calculate temperature from ADC value (linear scan over deltas).
 * @param adcValue Raw or tc-corrected 10-bit NTC ADC code (0..1023).
 * @return Temperature in tenths of °C, -200..800 (integer °C granularity).
 */
int16_t calc_temperature(uint16_t adcValue)
{
    if (adcValue <= TERMO_BASE_ADC)
        return TEMPERATURE_TABLE_START;

    uint16_t v = TERMO_BASE_ADC;
    uint16_t i = 0;

    for (; i < TERMO_DELTA_COUNT; i++)
    {
        uint8_t d = (uint8_t)((i & 1u) ? (termo_table[i >> 1] >> 4)
                                         : (termo_table[i >> 1] & 0x0Fu));
        uint16_t nv = (uint16_t)(v + d);
        if (adcValue < nv)
            break;
        v = nv;
    }

    return TEMPERATURE_TABLE_START + (int16_t)(i * TEMPERATURE_TABLE_STEP);
}

#endif /* defined(UART_DEBUG) || defined(PRINT_ADC) */

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

    // 3. PD2 — аналоговый вход (NTC), PD3 — Push-Pull 50MHz (делитель)
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xFF << 8)) | (0x3 << 12);
    GPIOD->BSHR = (1 << 3);  // PD3 = 1, делитель обесточен

    // 5. Сброс ADC1 через RCC (как делает SDK)
    RCC->APB2PRSTR |= (1 << 9);  // ADC1RST = 1
    RCC->APB2PRSTR &= ~(1 << 9); // ADC1RST = 0

    // 6. CTLR1: Independent mode, CALVOLSELECT=50% (бит 25), JAUTO (бит 10)
    ADC1->CTLR1 = (1 << 25) | (1 << 10);

    // 7. CTLR2: EXTSEL=7 (0x000E0000), EXTTRIG (бит 20), ADON (бит 0)
    ADC1->CTLR2 = 0x001E0001;

    // 8. RSQR1: L[3:0]=0 (1 канал в регулярной последовательности)
    ADC1->RSQR1 = 0;

    // 9. RSQR3: SQ1 = канал 3 (NTC на PD2)
    ADC1->RSQR3 = 3;

    // 10. SAMPTR2: время семплирования регулярного канала SQ1=CH3(NTC)
    ADC1->SAMPTR2 = (5 << 9) | (3 << 24);

    // 11. ISQR: JL=0 (1 инжектированный канал), JSQ4 = 8 (Vrefint на CH32V003)
    ADC1->ISQR = 8 << 15;

    // 12. SAMPTR1: время семплирования инжектированного канала (Rank1 = JSQ4)
    ADC1->SAMPTR1 = 3; // код 3 -> 30 циклов АЦП для Vrefint

    // 13. Задержка после включения ADC
    for (volatile int i = 0; i < 1000; i++)
        ;

    // 14. Сброс калибровки (RSTCAL = бит 3 в CTLR2)
    ADC1->CTLR2 |= (1 << 3);
    while (ADC1->CTLR2 & (1 << 3))
        ;

    // 15. Калибровка ADC (CAL = бит 2 в CTLR2)
    ADC1->CTLR2 |= (1 << 2);
    while (ADC1->CTLR2 & (1 << 2))
        ;

    ema_init(0);
}

/**
 * @brief Perform one verified ADC read: NTC temperature + Vrefint -> Vbat.
 *
 * Powers the NTC divider (PD3=0), waits for settling, starts regular+injected
 * conversion, reads results, then powers down the divider (PD3=1).
 * Updates raw_temperature_adc, vrefint_adc, voltage_ok, temperature_c
 * only after both
 * conversions complete successfully.
 * @return true if a fresh NTC/Vrefint pair was committed, false otherwise.
 */
bool adc_read(void)
{
    bool regular_ready;
    bool injected_ready;

    /* A new conversion invalidates the previous sample until the complete
     * NTC/Vrefint pair passes all acquisition checks below. */
    voltage_ok = false;

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
    regular_ready = (ADC1->STATR & (1 << 1)) != 0U;

    // Ожидаем завершения инжектированного преобразования (JEOC, бит 2), с таймаутом
    timeout = ADC_WAIT_TIMEOUT;
    while (!(ADC1->STATR & (1 << 2)) && --timeout)
        ;
    injected_ready = (ADC1->STATR & (1 << 2)) != 0U;

    // Отключаем делитель напряжения: PD3 = 1 (ток через делитель не течёт, экономия заряда)
    GPIOD->BSHR = (1 << 3);

    /* Never pass a previous ADC register value to the thermal or voltage
     * controllers.  A fresh NTC rail value is valid data and is deliberately
     * not rejected here: the thermal limiter owns that failsafe decision. */
    if (!regular_ready || !injected_ready)
        return false;

    uint16_t new_raw_temperature_adc = ADC1->RDATAR;
    uint16_t vrefint_raw = ADC1->IDATAR1;

    /* A 1S cell should produce roughly 2.4..4.8 V in this measurement path,
     * corresponding to Vrefint codes 256..512.  Reject implausible raw data
     * before it can seed or contaminate the EMA or bypass undervoltage cut-off. */
    if ((vrefint_raw < 256u) || (vrefint_raw > 512u))
        return false;

    /* Commit all values together.  get_exact_temp_adc() intentionally reads
     * the newly committed raw value so calibration and limiter see the same
     * sample.  Voltage stays in raw Vrefint code domain — no division here. */
    vrefint_adc = ema_update(vrefint_raw);
    voltage_ok = true;
    raw_temperature_adc = new_raw_temperature_adc;

#if defined(UART_DEBUG) || defined(PRINT_ADC)
    // Расчет температуры по таблице (с коррекцией tc) — только для отладки
    temperature_c = calc_temperature(get_exact_temp_adc());
#endif

#ifdef PRINT_ADC
    // Выводим результат
    EE_PRINTF("%04d, %04d -> Vbat: %ld mV | Temp: %d.%d C\r\n",
              vrefint_adc, raw_temperature_adc, (long)vbat_to_mv_calibrated(),
              temperature_c / 10, temperature_c % 10);
#endif

    return true;
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

    /* 3. PD2 и PD3 переводим в Analog Input — высокое импедансное состояние (Hi-Z) */
    GPIOD->CFGLR &= ~(0xFF << 8);  /* PD2/PD3: 0b0000 = Analog Input (Hi-Z) */
}

#if defined(UART_DEBUG) || defined(PRINT_ADC)

/**
 * @brief Пересчёт кода Vrefint в милливольты (точно, без libgcc).
 * @param vrefint_adc Код Vrefint (валидный диапазон 256..512).
 * @return Напряжение в мВ = floor(1228800/adc), 0 при adc==0.
 * @details Школьное деление по битам частного (14 итераций). Точно при
 * vrefint_adc >= 129 (частное < 16384); это покрывает и фактический
 * диапазон выборки 256..512, и его возможный калибровочный сдвиг ±22
 * (окно VC_CLIP_* в adc.h).
 * Используется только для отладочного вывода — защита работает
 * непосредственно в кодах АЦП.
 */
__attribute__((noinline)) uint16_t vbat_to_mv(uint16_t vrefint_adc)
{
    uint32_t rem = VREF_NUMERATOR;
    uint16_t q = 0u;

    if (vrefint_adc == 0u)
        return 0u;

    for (int8_t i = 13; i >= 0; i--)
    {
        uint32_t t = (uint32_t)vrefint_adc << i;
        if (rem >= t)
        {
            rem -= t;
            q |= (uint16_t)(1u << i);
        }
    }
    return q;
}

/**
 * @brief Convert the current valid calibrated Vrefint sample to mV.
 * @return Calibrated battery voltage in mV, or 0 without a valid sample.
 */
uint16_t vbat_to_mv_calibrated(void)
{
    if (!voltage_ok)
        return 0u;

    int32_t effective_code = (int32_t)vrefint_adc - (int32_t)ob_GetVc();
    /* vbat_to_mv() uses 14 quotient bits; with a valid raw sample and the
     * documented vc range the normal lower bound is 129.  Treat an invalid
     * Option Bytes value or an impossible effective code as no reading. */
    if (effective_code < 129)
        return 0u;

    return vbat_to_mv((uint16_t)effective_code);
}

#endif /* defined(UART_DEBUG) || defined(PRINT_ADC) */

/* ============ Защита по разряду батареи (в кодах Vrefint) ============
 * vbat_mv = floor(1228800/adc); Vrefint растёт при падении напряжения.
 * Все пороги переведены в коды времени компиляции (MV_LT/MV_GE):
 *   vbat < mV  ⟺  adc >= MV_LT(mV)   (низкое напряжение — код высокий)
 *   vbat >= mV ⟺  adc <= MV_GE(mV)
 * Калибровочный vc (int8, коды) входит как e = adc - vc.
 * Смысл индексов сохранён из исходника:
 *   [0] = критический разряд → принудительный сон
 *   [1] = только CMODE_MOON
 *   [2] = CMODE_MIN
 *   [3] = CMODE_MID0
 *   [4] = CMODE_MID
 *   [5] = восстановление / без ограничений (CMODE_TURBO)
 */
static const uint16_t swt_cut[6] =
{   /* vbat < mV ⟺ adc >= это значение */
    MV_LT(2700), MV_LT(2750), MV_LT(2800), MV_LT(2900), MV_LT(3000), MV_LT(3100)
};
/* vbat > mV ⟺ adc <= это значение (потолок яркости UI1). */
static const uint16_t ceil_cap[6] =
{
    MV_GE(2501), MV_GE(2821), MV_GE(2931), MV_GE(3081), MV_GE(3191), MV_GE(3301)
};
#define CUT_RECOVERY_MV 3101u   /* vbat > 3100 мВ ⟺ adc <= MV_GE(3101)=396 */

/**
 * @brief Вычисляет максимально допустимый режим UI2 по текущему напряжению.
 * @param eff_vcode Эффективный код Vrefint (с учётом калибровки vc).
 * @return Индекс максимального режима (CMODE_MOON..CMODE_TURBO).
 */
static uint8_t calc_volt_max_mode(int32_t eff_vcode)
{
    /* Индексы 1..5 совпадают со swt_cut[1..5]; индекс 0 в этом цикле не
     * используется (итерация с CMODE_MOON=1). */
    for (uint8_t i = CMODE_MOON; i < CMODE_TURBO; i++)
        if (eff_vcode >= (int32_t)swt_cut[i]) return i;
    return CMODE_TURBO;
}

/* ============ Потолок яркости UI1 по напряжению (ramping) ============
 * Аналог AVR adc.c limiter[2][6] + ratchet_max_step().
 * step_max_tbl — 9-битные шаги (0..511), масштабированные из AVR 8-бит.
 * volt_step_ratchet монотонно убывает (MIN) до явного безопасного восстановления. */
static const uint16_t step_max_tbl[6] = {120, 240, 310, 400, 463, 511};

/* Код Vrefint, представляющий «критически низкое напряжение» при отсутствии
 * валидной выборки (voltage_ok == false).  Заменяет прежний маркер
 * vbat_mv == 0: все пороги «vbat < mv» срабатывают, «vbat > mv» — нет. */
#define VREF_SENTINEL 1023u

/** @brief Чистый расчёт потолка по текущему коду Vrefint (без побочных эффектов). */
static uint16_t calc_step_ceiling(void)
{
    int16_t vc = ob_GetVc();
    uint16_t vcode = voltage_ok ? vrefint_adc : VREF_SENTINEL;
    uint8_t i = 6;
    while (i > 0)
    {
        i--;
        if ((int32_t)vcode - vc <= (int32_t)ceil_cap[i]) break;
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

extern volatile uint8_t need_sleep;

/**
 * @brief Необратимое выключение: обесточить нагрузку и уйти в вечный STANDBY.
 *
 * Защита от глубокого разряда. Обесточивает нагрузку (LED через off_red/off_grn,
 * DAC PD0), переводит I2C и делитель NTC в Hi-Z, отключает AWU/LSI и все
 * источники пробуждения (кнопка EXTI3, AWU EXTI9 и прочие EXTI-линии).
 * Вечный цикл STANDBY: функция не возвращается (в отличие от AWU_Sleep_Now(),
 * который мог вернуться и отменить PDDS).
 */
__attribute__((noinline, noreturn)) void down_forever(void)
{
    /* 1. Обесточить нагрузку: AUX-светодиоды погасить через штатные ф-ции
     *    (уважают особенность PC6/PC7), питание DAC (PD0)=0. */
    off_red();
    off_grn();
    dac_pwrpin_off();
 
    /* 2. I2C-линии PC1/PC2, кнопка PC3 (отключение pull-up для предотвращения
     *    утечки при зажатой кнопке на разряженном АКБ) и делитель NTC PD2/PD3 → Hi-Z Analog. */
    GPIOC->OUTDR &= ~(1 << 3);
    GPIOC->CFGLR &= ~((0x0F << 4) | (0x0F << 8) | (0x0F << 12));
    GPIOD->CFGLR &= ~(0xFF << 8);

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
 * @brief Battery undervoltage protection (call at 30 Hz), в кодах Vrefint.
 *
 * For UI1 (ramping): step-down brightness at each threshold, hard cutoff at 2700 mV.
 * For UI2 (discrete): reduce mode automatically, hard cutoff if already at mode 0.
 * Monotonic: volt_stop_mode only decreases, recovers on reboot.
 *
 * Все сравнения выполняются над кодом Vrefint (без делений); калибровочный
 * vc учитывается как сдвиг кода.  При voltage_ok == false используется
 * VREF_SENTINEL — аналог прежнего vbat_mv == 0 (критично низкое).
 */
void scan_voltage(void)
{
    static uint8_t voff_count = 0;
    static uint8_t voff_count_off = 0;

    int16_t vc = ob_GetVc();
    uint16_t vcode = voltage_ok ? vrefint_adc : VREF_SENTINEL;
    int32_t eff_vcode = (int32_t)vcode - vc;

    volt_max_mode = calc_volt_max_mode(eff_vcode);

    if (voltage_ok)
    {
        if (eff_vcode >= (int32_t)MV_LT(3100))
            beacon_low_voltage = true;
        else if (eff_vcode <= (int32_t)MV_GE(3200))
            beacon_low_voltage = false;
    }

    if (!driver_is_active()) return;

    /* 1. Общая защита от глубокого разряда (< 2.70 В) для всех UI */
    if (eff_vcode >= (int32_t)swt_cut[0])
    {
        if (++voff_count_off > 3)
        {
            voff_count_off = 0;
            down_forever();
        }
        return;
    }
    voff_count_off = 0;

#ifdef USE_EX_AUX
    exaux(eff_vcode);
#endif

    /* 2. Ступенчатое снижение яркости (UI1 / UI2) */
    uint8_t ui_index = ui_table_idx;

    /* Ramping UI (UI1, тактический, меню) */
    if (ui_index <= 1u || ui_index > 3u)
    {
        /* Step-down brightness on voltage sag */
        if (eff_vcode >= (int32_t)swt_cut[down_count])
        {
            if (++voff_count > 3)
            {
                voff_count = 0;
                if (down_count)
                {
                    down_count--;
                    step_user -= (step_user >> 2);
                    set_current(step_user, SS_SLOW);
                    aux_err(AWU_IMMEDIATELY);       // сигнализируем о сбросе
                }
            }
            return;
        }

        voff_count = 0;
        /* Recovery: voltage above top threshold (vbat > 3100) */
        if (eff_vcode <= (int32_t)MV_GE(CUT_RECOVERY_MV))
            down_count = 5;
    }
    /* Discrete UI (mode-based UI2: ui_index 2 или 3) */
    else
    {
        if (mode_current && (eff_vcode >= (int32_t)swt_cut[mode_current - 1]))
        {
            if (++voff_count > 3)
            {
                voff_count = 0;
                --mode_current;
                if (volt_stop_mode > mode_current)
                    volt_stop_mode = mode_current;
                if (mode_current)
                {
                    set_current(get_stepi(mode_current), SS_SLOW);
                    aux_err(AWU_IMMEDIATELY);       // сигнализируем о сбросе
                }
            }
            return;
        }

        voff_count = 0;
    }
}

/** @brief Get battery indicator flash count (0-6) based on Vbat (в кодах Vrefint). */
uint8_t bat_get_flashes(void) {
	static const uint16_t thresholds[] = {MV_LT(3000), MV_LT(3200), MV_LT(3400),
	                                      MV_LT(3600), MV_LT(3800), MV_LT(4000)};
	int32_t eff_vcode = (int32_t)(voltage_ok ? vrefint_adc : VREF_SENTINEL) - ob_GetVc();
	for (uint8_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++) {
		if (eff_vcode >= (int32_t)thresholds[i]) return i;
	}
	return 6;
}
