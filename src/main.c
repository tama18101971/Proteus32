/**
 * @file main.c
 * @brief Entry point and main loop for Proteus32 firmware
 *
 * CH32V003-based flashlight with:
 * - Red/green AUX LEDs via GPIO
 * - NTC temperature sensing (ADC)
 * - Button control (EXTI)
 * - AWU (Auto Wake-Up) for periodic tasks and sleep
 * - TIM2 (1 ms system tick)
 * - EEPROM emulation in Flash (settings)
 *
 * Main loop:
 * 1. Keep fast thermal supervision while the applied brightness is high or
 *    the limiter is active.
 * 2. Use an AWU_4S-owned standby loop for low-power thermal supervision.
 * 3. Keep AUX/strobe AWU ownership separate from thermal supervision.
 * 4. Timer tasks: 160 Hz (UI + softstart), 30 Hz (fast thermal scan),
 *    2 Hz (debug), 5 s (sleep/persistence; EE commit on config diff only:
 *    menus, slots, explicit saves. Session brightness lives in SRAM and
 *    survives Standby — no flash writes on everyday on/off).
 */
#include <stdint.h>
#include <stdbool.h>
#include "led_gpio.h"
#include "i2c.h"
#include "dac.h"
#include "button_gpio.h"
#include "button.h"
#include "aux_leds.h"
#include "app_data.h"
#include "ui.h"
#include "adc.h"
#include "timers.h"
#include "limiters.h"
#include "ob.h"
#include "array.h"
#include "mini_printf.h"
#include <string.h>

void __libc_init_array(void) {}

/* ── Stack canary ────────────────────────────────────────────────────
 * Слово-канарейка в НИЖНЕМ слове стековой области (_susrstack задаёт
 * Link.ld, стек растёт вниз от _eusrstack). Запись в канарейку означает,
 * что потребление стека достигло предела 512 Б — практически всегда это
 * переполнение с затиранием соседней .bss. Консервативная сигнализация:
 * ложное срабатывание на ровно полном стеке даёт безопасный отказ
 * (гашение нагрузки + программный сброс), молчаливое переполнение
 * исключается. Проверка — на 5-с тике бодрствования. */
extern uint32_t _susrstack; /* linker-provided (Link.ld) */
#define STACK_CANARY_WORD 0xC0DE5A7Au

volatile wakeup_source_t wakeup_source = WS_NONE; /* Источник последнего пробуждения */

volatile uint8_t need_sleep = false; /* Флаг необходимости перейти в режим сна */

/* Тень последнего сохранённого в flash полезного груза настроек. Запись
 * выполняется только при реальном отличии текущих данных от тени — "чётные"
 * переключения настроек не приводят к лишней записи. Сессионная яркость
 * (step_default/mode_default) живёт в SRAM и EE не трогает, поэтому при
 * повседневных включениях/выключениях записей во flash нет. Коммит — на
 * 5-сек тике по diff с тенью; серия правок между тиками склеивается в одну
 * запись. Служебные поля записи (seq/version/crc) хранит библиотека. */
static app_cfg_t cfg_shadow;

/* getBright_CRT_9to12Bit(322) = 1018, or 24.86% of DAC_MAX_VALUE.
 * The strict '>' below therefore enters fast supervision at step 323:
 * DAC=1028, approximately 25.1% of maximum output. */
#define THERMAL_FAST_STEP       322U

typedef enum
{
    SLEEP_OWNER_NORMAL = 0,
    SLEEP_OWNER_THERMAL
} sleep_owner_t;

static sleep_owner_t sleep_owner = SLEEP_OWNER_NORMAL;

/**
 * @brief Обработчик аппаратной ошибки ядра (HardFault).
 *
 * Штатный слабый обработчик SDK зациклен (startup_ch32v00x.S), при этом
 * силовая часть остаётся включённой до разряда АКБ. Здесь нагрузка
 * безусловно гасится: выводы светодиодов переводятся в аналоговый Hi-Z
 * (GPIO_PinConfig inline, стек не используется сверх пролога), питание
 * DAC (PD0) активно притягивается к 0, затем выполняется программный
 * сброс через PFIC. Функция управления не возвращает.
 */
void HardFault_Handler(void) __attribute__((noreturn));
void HardFault_Handler(void)
{
    /* Один masked-write: PC4/PC6/PC7 -> Analog Input.  Промежуточные
     * значения CFGLR никем не читаются, итог совпадает с тремя
     * GPIO_PinConfig(CFG_IN_ANALOG) по отдельности — на RV32EC это на
     * две пары load/mask/store дешевле. */
    GPIOC->CFGLR &= ~(((uint32_t)0xFu << (4 * 4)) | ((uint32_t)0xFu << (4 * 6)) |
                      ((uint32_t)0xFu << (4 * 7)));
    GPIOC->BCR = GPIO_Pin_4 | GPIO_Pin_6 | GPIO_Pin_7;
    GPIOD->BCR = (1u << 0); /* PD0: питание DAC off */
    NVIC_SystemReset();     /* Программный сброс ядра (core_riscv.h) */
    for (;;)
        ;
}

/**
 * @brief Check whether frequent thermal supervision is required.
 * @return true for high applied brightness or an active thermal reduction.
 */
static bool thermal_fast_required(void)
{
    if (!driver_is_active())
        return false;

    return (step_tc > THERMAL_FAST_STEP) || (limiter != 0U);
}

/**
 * @brief Check whether the main driver may enter the slow thermal sleep loop.
 * @return true when low-power sampling can safely own or follow the AWU.
 *
 * Активный AUX FSM термосну не мешает: пока паттерн идёт, cadence задают
 * его окна (handle_aux() перевооружает AWU на каждом wake), а замеры
 * АКБ/термо выполняются на каждом aux-пробуждении; при тихом AUX — AWU_4S.
 */
static bool thermal_sleep_allowed(void)
{
#if USE_LSI_GUARD
    /* Без LSI таймер AWU не тикает: термосон AWU_4S не проснулся бы и
     * фонарь остался в STANDBY без термоконтроля и отсечки разряда.
     * Деградация: MCU остаётся на TIM2 (30 Гц опрос) — контроль полный,
     * цена — потребление бодрствования. */
    if (!awu_ready)
        return false;
#endif
    return driver_is_active() &&
           (driver_mode != MODE_BLOCK_LIGHT) &&
           !thermal_fast_required() &&
           !ss_is_active() &&
           !button_sequence_active();
}

/**
 * @brief Acquire a fresh ADC pair and run voltage/thermal protection.
 * @return true when a fresh pair was accepted, false on acquisition failure.
 */
static bool sample_and_scan(void)
{
    if (adc_read())
    {
        if (driver_mode != MODE_STROB)
        {
            scan_limiters();
            scan_voltage();
        }
        return true;
    }

    /* A timeout is not a temperature sample.  Use the unchanged limiter's
     * rail failsafe instead of reusing the previous global ADC value. */
    if (driver_mode != MODE_STROB)
    {
        voltage_ok = false;
        scan_limiters_fault();
        scan_voltage();
    }

    return false;
}

/**
 * @brief Восстановление I2C/DAC после выхода из STANDBY.
 *
 * Standby сбрасывает тактирование периферии: для работы DAC нужен
 * перезапуск I2C после подъёма питания PD0.  Одна noinline-функция вместо
 * трёх инлайн-копий последовательности в main() (LTO иначе дублирует её
 * по месту).  adc_init() не входит сюда — третья точка вызова её не имеет.
 */
__attribute__((noinline)) static void wake_i2c(void)
{
    dac_pwrpin_restore();
    i2c_init(400000);
    wait_ms(2);
}

/**
 * @brief Точка входа и главный цикл прошивки.
 *
 * Инициализирует все периферийные модули, затем циклически обслуживает:
 * - пробуждение по кнопке (восстановление периферии),
 * - пробуждение по AWU (термоконтроль или AUX-команды),
 * - режим сна (STANDBY),
 * - таймерные задачи (160 Гц, 30 Гц, 2 Гц, 5 с).
 */
int main(void)
{
    wait_ms(250);

#ifdef UART_DEBUG
    uart_init(UART_DEBUG_BAUD);
#endif

    EE_PRINTF("\r\nBoot...\r\n");

    sys_init();

    AWU_init();
    AWU_int_init();
    AWU_set_delay(AWU_15MS);

    led_init();
    button_gpio_init();

    adc_init();

    /* DAC + I2C */
    dac_pwrpin_init();
    dac_pwrpin_on();
    i2c_init(400000);
    dac7571_set_voltage(0);
    OFF_POWER();

    (void)ob_Init();
    if (adc_read())
    {
        /* Гистерезисный флаг низкого заряда: при загрузке достаточно
         * однократного обновления на валидной выборке. */
        (void)beacon_low_voltage_update();
    }
    if (wl_load() != WL_OK)
    {
        wl_init();
        ee_defaults();
        wl_mark_dirty();
        (void)wl_commit_if_dirty();
    }
    cfg_shadow = EE;

    /* Инициализация FSM кнопки и UI */
    static const btn_config_t btn_cfg = {
        .hold_ticks = BTN_HOLD_TICKS,
        .repeat_ticks = BTN_REPEAT_TICKS,
        .release_ticks = BTN_RELEASE_TICKS,
        .max_clicks = BTN_MAX_CLICKS,
        .long_click_ticks = BTN_LONG_CLICK_TICKS,
    };
    button_init(&btn_cfg);
    ui_init();

    /* Вход в меню при подаче питания с зажатой кнопкой. Выбор меню — по
     * напряжению питания (см. enter_menu_at_boot, как в оригинале Proteus IID). */
    enter_menu_at_boot();

    need_sleep = true;

    reset_limiters();

    /* Стартовая засветка канарейки стека: до главного цикла */
    *(volatile uint32_t *)&_susrstack = STACK_CANARY_WORD;

    EE_PRINTF("[TSET] setpoint=%u kp_shift=%u ki_decim=%u ki_recov=%u offset=%u\r\n",
              get_thermal_setpoint(), config.kp_left_shift,
              config.ki_decimation_shift, config.ki_recov_shift, config.t_anticipate_offset);

    while (1)
    {
        /* 1./2. Обработка пробуждения: снапшот и очистка выполняются ДО
         * ветвления. Прерывание, вклинившееся между проверкой флага и его
         * сбросом, иначе теряется (гонка кнопка/AWU). Пара "прочитал-
         * очистил" выполняется при замаскированном MIE: ISR, вклинившийся
         * в окно, не затирается записью WS_NONE и будет обработан на
         * следующей итерации. При совместном пробуждении кнопка
         * приоритетна, AWU-тик просто пропускается. */
        if (wakeup_source != WS_NONE)
        {
            uint32_t mie_save;
            __asm volatile("csrrci %0, mstatus, 8" : "=r"(mie_save) :: "memory");
            wakeup_source_t ws = wakeup_source;
            wakeup_source = WS_NONE;
            if (mie_save & 0x08u)
            {
                __asm volatile("csrsi mstatus, 8" ::: "memory");
            }

            /* 1. Пробуждение по кнопке: восстанавливаем периферию */
            if (ws == WS_BUTTON && is_sleep_mode)
            {
                /* При активном AUX AWU не обезоруживаем: вооружённое окно
                 * переживает STANDBY+SystemInit (как OFF-маяк при owner
                 * NORMAL), паттерн продолжится через WS_AWU → handle_aux()
                 * в бодрствовании.  Иначе метроном AUX глохнет навсегда:
                 * перевооружать некому, а sleep-блок видит aux_is_active()
                 * и не ставит AWU_4S → вечный STANDBY с включённым
                 * драйвером и залипшим LED. */
                if ((sleep_owner == SLEEP_OWNER_THERMAL) && !aux_is_active())
                    AWU_OFF();
                SystemInit();
                /* Вернуть LSI: в чистом OFF-сне он был остановлен (AWU_stop(true)),
                 * а маяк/строб/термо-сон тактируются от него. Если LSI уже работает,
                 * ожидание LSIRDY в AWU_init() завершается сразу. */
                AWU_init();
#ifdef UART_DEBUG
                uart_init(UART_DEBUG_BAUD);
#endif
                is_sleep_mode = false;
                need_sleep = false;
                wake_i2c();
                stop_aux_if(IDM_BEACON, false);
                adc_init();
                if (!is_aux_mode(IDM_STROBE))
                {
                    if (sample_and_scan())
                    {
#if USE_BEACON_ADC_GUARD
                        /* АЦП доказанно жив + пользователь присутствует:
                         * маяку можно возвращаться на следующем OFF-сне. */
                        beacon_adc_latch_reset();
#endif
                    }
                }
                tim2_start();
                sleep_owner = SLEEP_OWNER_NORMAL;
            }
            /* 2. AWU wake: thermal sleep and AUX commands have separate owners. */
            else if (ws == WS_AWU)
            {
                if ((sleep_owner == SLEEP_OWNER_THERMAL) && is_sleep_mode)
                {
                    AWU_OFF();
                    wake_i2c();
                    adc_init();

                    /* AUX FSM задаёт cadence пробуждений, пока паттерн активен:
                     * каждый wake обязан продвинуть один переход и перевооружить
                     * AWU следующим окном (симметрично AUX-ветке ниже).  При
                     * неактивном AUX это no-op: off_aux_cmd() очистил
                     * p_aux/EnableArrayProcessing — pending=false, и sleep-блок
                     * ниже перевооружит AWU_4S. */
                    handle_aux();

                    bool sample_ok = sample_and_scan();
                    EE_PRINTF("[TWAKE] ok=%u T=%u F=%d I=%d L=%u V=%umV\r\n",
                              (unsigned)sample_ok, get_exact_temp_adc(),
                              state.filtered_temp, state.integral, limiter,
                              vbat_to_mv_calibrated());
                    if (!sample_ok || !thermal_sleep_allowed())
                    {
                        is_sleep_mode = false;
                        need_sleep = false;
                        sleep_owner = SLEEP_OWNER_NORMAL;
                        tim2_start();
                    }
                    else
                    {
                        /* Keep the request set; the common sleep block below
                         * stops ADC/TIM2 and arms the next AWU_4S interval. */
                        need_sleep = true;
                    }
                }
                else
                {
                    /* Strobe owns the DAC and bypasses scan_voltage(), so the
                     * 2.7 V deep-discharge latch is explicitly restored here:
                     * a read-only battery sample (no DAC writes) aborts the
                     * strobe into the permanent protective state. */
                    if (is_aux_mode(IDM_STROBE))
                    {
#if USE_STROBE_ADC_GUARD
                        uint8_t batt = strobe_battery_state();
                        if (batt == STROBE_BATT_LOW)
                        {
                            off_aux_cmd(false);
                            /* down_forever() EE не коммитит: запись при
                             * критическом разряде опаснее потери последних
                             * изменений настроек. */
                            down_forever();
                        }
                        else if (batt == STROBE_ADC_DEAD)
                        {
                            /* Восстанавливаемый останов: отказ измерений не
                             * доказывает разряд АКБ, поэтому без down_forever.
                             * off_strobe() корректно выходит из MODE_STROB
                             * (сессия восстановлена), пользователь может
                             * запустить строб снова — счётчики серий свежие. */
                            off_aux_cmd(false);
                            off_strobe();
                        }
#else
                        if (strobe_voltage_low())
                        {
                            off_aux_cmd(false);
                            /* down_forever() EE не коммитит: запись при
                             * критическом разряде опаснее потери последних
                             * изменений настроек. */
                            down_forever();
                        }
#endif
                    }
                    if (beacon_is_cycle_start())
                    {
                        beacon_battery_check();
                    }
                    if (aux_needs_dac())
                    {
                        wake_i2c();
                    }
                    handle_aux();
                }
            }
        }

        /* 3. Сон: AUX wake must be processed above before returning to
         * STANDBY, otherwise a strobe would repeatedly sleep on one phase. */
        /* A strobe normally sleeps between AWU phases.  A button wake must
         * instead leave the FSM running through the full click timeout;
         * otherwise the first press would abort or be lost before UI can
         * choose between OFF (one click) and next pattern (two clicks). */
        /* Tactical mode bypasses the button FSM and must remain awake while
         * the button is held.  Its release cannot wake the MCU because EXTI3
         * is configured for the press (falling) edge only. */
        if ((ui_table_idx == 0U) && Pressed)
        {
            need_sleep = false;
            tick_5s_reset();
        }

#if USE_LSI_GUARD
        /* LSI умер при активном AUX-режиме (транзиент EMI при бодрствовании
         * или неудачный рестарт AWU_init() при пробуждении кнопкой):
         * гасим корректно. off_strobe() восстанавливает сессию и выходит
         * из MODE_STROB; для остальных AUX-владельцев достаточно
         * off_aux_cmd(false). AWU-владение дальше не восстанавливается —
         * последующие гейты старта держат AUX выключенным. */
        if (!awu_ready && aux_is_active())
        {
            off_aux_cmd(false);
            if (driver_mode == MODE_STROB)
                off_strobe();
        }
#endif

        bool strobe_sleep = is_aux_mode(IDM_STROBE) && !button_sequence_active();
        bool thermal_sleep = (need_sleep &&
                              (sleep_owner == SLEEP_OWNER_THERMAL));

        if (thermal_sleep && !thermal_sleep_allowed())
        {
            if (driver_is_active())
            {
                need_sleep = false;
                sleep_owner = SLEEP_OWNER_NORMAL;
                thermal_sleep = false;
            }
            else
            {
                sleep_owner = SLEEP_OWNER_NORMAL;
                thermal_sleep = false;
            }
        }

        if (Pressed)
        {
            need_sleep = false;
        }

        if (need_sleep || strobe_sleep)
        {
            if (!driver_is_active() && !aux_needs_dac())
            {
                OFF_POWER();
            }

            /* Единственная точка выключения АЦП (см. adc_stop): строб-защита
             * и маяк АЦП не гасят — иначе следующий sample_and_scan() в
             * бодрствующем состоянии прочитал бы обесточенный АЦП.  Каждый
             * пробуждающий wake снова вызывает adc_init(). */
            adc_stop();
            tim2_stop();

            if (thermal_sleep)
            {
                /* Пока активен AUX-паттерн, cadence владеет AUX FSM: AWU уже
                 * вооружён handle_aux() следующим окном.  Перевооружаем
                 * 4-секундный интервал только при тихом AUX. */
                if (!aux_is_active())
                {
                    AWU_OFF();
                    AWU_set_delay(AWU_4S);
                    AWU_ON();
                }
            }
            else if (!is_aux_mode(IDM_STROBE))
            {
                start_aux_beacon();
                /* Всё выключено и маяк не стартовал (выкл или замьючен):
                 * AWU никому не принадлежит — останавливаем AWU + LSI.
                 * Даташит CH32V003DS0, Table 3-8: Standby без LSI =
                 * 7.6 мкА typ вместо 9.1 мкА (LSI on) при VDD=3.3В.
                 * LSI возвращается в AWU_init() при пробуждении по кнопке. */
                if (!driver_is_active() && !aux_is_active())
                {
                    AWU_stop(true);
                }
            }
            AWU_Sleep_Now();
            continue;
        }

        /* 4. Таймерные задачи */
        if (tick_160hz())
        {
            button_tick();
            ui_process();
            ss_scan();
        }

        if (tick_30hz())
        {
            /* The strobe owns the DAC waveform.  Do not let ADC/limiters
             * write a regular brightness value during an input window.
             * Scan whenever the driver is active: the UI1 ramping ceiling
             * (ratchet) needs fresh voltage from step 1, and a held button
             * blocks the AWU_4S loop (button_sequence_active()), so the old
             * step_tc > THERMAL_FAST_STEP gate left a monitoring blind zone
             * during low-brightness ramps. */
            if (driver_is_active() && !is_aux_mode(IDM_STROBE))
            {
                (void)sample_and_scan();
            }
        }

#ifdef UART_DEBUG
        if (tick_2hz())
        {
            uint16_t out = get_step_tc(step_user);
            EE_PRINTF("[TSTAT] T=%u F=%d I=%d L=%d P=%u S=%u V=%umV\r\n",
                      get_exact_temp_adc(), state.filtered_temp,
                      state.integral, state.prev_limiter, out, step_user,
                      vbat_to_mv_calibrated());
        }
#endif

        if (tick_5s())
        {
            /* Переполнение стека (канарейка в нижнем слове затёрта):
             * обесточить нагрузку и перезагрузиться через штатный
             * HardFault-путь. */
            if (*(volatile uint32_t *)&_susrstack != STACK_CANARY_WORD)
                HardFault_Handler();

            /* Deferred persistence: OB flushes only when its cache is dirty
             * (ob.c); EE commits only when the working copy differs from the
             * shadow — an even number of toggles between ticks restores the
             * old value and produces no flash write. Everyday on/off cycles
             * keep EE untouched (session brightness lives in SRAM), so the
             * memcmp below stays equal during normal use. */
            ob_Flush();
            if (memcmp(&cfg_shadow, WL_DATA_AS(app_cfg_t), sizeof(app_cfg_t)) != 0)
            {
                wl_mark_dirty();
                if (wl_commit_if_dirty() == WL_OK)
                    cfg_shadow = EE;
            }
            if (driver_is_active())
            {
                if (thermal_sleep_allowed())
                {
                    /* Low-power scans are intentionally sparse.  Take one
                     * final fresh sample before stopping ADC/TIM2 so a stale
                     * low-temperature value cannot authorize standby. */
                    bool sample_ok = sample_and_scan();
                    if (sample_ok && thermal_sleep_allowed())
                    {
                        EE_PRINTF("[TSLEEP] S=%u L=%u\r\n",
                                  step_tc, limiter);
                        sleep_owner = SLEEP_OWNER_THERMAL;
                        need_sleep = true;
                    }
                    else
                    {
                        sleep_owner = SLEEP_OWNER_NORMAL;
                        need_sleep = false;
                    }
                }
                else
                {
                    /* High power, active limiting, soft-start, or button
                     * activity keeps the MCU awake.  AUX ownership no
                     * longer does: thermal sleep follows its cadence. */
                    sleep_owner = SLEEP_OWNER_NORMAL;
                    need_sleep = false;
                }
            }
            else if (!is_aux_mode(IDM_STROBE))
            {
                sleep_owner = SLEEP_OWNER_NORMAL;
                need_sleep = true;
            }
        }

        /* 5. Light Sleep (WFI) between 1 ms TIM2 ticks when awake */
        if (!is_sleep_mode && !need_sleep && !strobe_sleep)
        {
            __WFI();
        }
    }
}
