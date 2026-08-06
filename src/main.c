/**
 * @file main.c
 * @brief Entry point and main loop for Proteus003 firmware
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
 *    2 Hz (debug), 5 s (sleep/persistence).
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

volatile wakeup_source_t wakeup_source = WS_NONE; /* Источник последнего пробуждения */

volatile uint8_t need_sleep = false; /* Флаг необходимости перейти в режим сна */

/* Тень последнего сохранённого в flash состояния EE. Запись выполняется только
 * при реальном отличии текущих данных от тени — "чётные" переключения настроек
 * в пределах 5-сек окна отката не приводят к лишней записи в flash. */
static struct ee_data ee_shadow;

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
 * @brief Check whether frequent thermal supervision is required.
 * @return true for high applied brightness or an active thermal reduction.
 */
static bool thermal_fast_required(void)
{
    if (driver_mode != MODE_ON)
        return false;

    return (step_tc > THERMAL_FAST_STEP) || (limiter != 0U);
}

/**
 * @brief Check whether the main driver may enter the slow thermal sleep loop.
 * @return true only when low-power sampling can safely own the AWU.
 */
static bool thermal_sleep_allowed(void)
{
    return (driver_mode == MODE_ON) &&
           !thermal_fast_required() &&
           !ss_is_active() &&
           !button_sequence_active() &&
           !aux_is_active();
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
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();

    wait_ms(3000);

#ifdef WL_FLASH_DEBUG
    uart_init(460800);
#endif

    EE_PRINTF("\r\n[MAIN] Boot...\r\n");

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

    ob_Init();
    (void)adc_read();
    if (!read_ee())
    {
        init_ee();
        ee_defaults();
    }
    memcpy(&ee_shadow, &ee, EE_STRUCT_SIZE);

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
    if (Pressed)
        enter_menu_at_boot();

    //start_aux_beacon();

    need_sleep = true;

    reset_limiters();

    EE_PRINTF("[LIMITER INIT] setpoint=%u kp_shift=%u ki_shift=%u ki_decim=%u offset=%u\r\n",
              BASE_TEMPERATURE + EE.usr.t_calibr, config.kp_left_shift, config.ki_left_shift,
              config.ki_decimation_shift, config.t_anticipate_offset);

    while (1)
    {
        /* 1. Пробуждение по кнопке: восстанавливаем периферию */
        if ((wakeup_source == WS_BUTTON) && is_sleep_mode)
        {
            if (sleep_owner == SLEEP_OWNER_THERMAL)
                AWU_OFF();
            SystemInit();
            SystemCoreClockUpdate();
#ifdef WL_FLASH_DEBUG
            uart_init(460800);
#endif
            is_sleep_mode = false;
            need_sleep = false;
            wakeup_source = WS_NONE;
            dac_pwrpin_restore();
            i2c_init(400000);
            stop_aux_if(IDM_BEACON, false);
            adc_init();
            if (!is_aux_mode(IDM_STROBE))
            {
                (void)sample_and_scan();
            }
            tim2_start();
            sleep_owner = SLEEP_OWNER_NORMAL;
        }

        /* 2. AWU wake: thermal sleep and AUX commands have separate owners. */
        if (wakeup_source == WS_AWU)
        {
            wakeup_source = WS_NONE;
            if ((sleep_owner == SLEEP_OWNER_THERMAL) && is_sleep_mode)
            {
                AWU_OFF();
                dac_pwrpin_restore();
                i2c_init(400000);
                adc_init();

                bool sample_ok = sample_and_scan();
                EE_PRINTF("[THERMAL WAKE] ok=%u T=%u F=%d I=%d L=%u V=%umV\r\n",
                          (unsigned)sample_ok, raw_temperature_adc,
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
                if (aux_needs_dac())
                {
                    dac_pwrpin_restore();
                    i2c_init(400000);
                }
                handle_aux();
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

        bool strobe_sleep = is_aux_mode(IDM_STROBE) && !button_sequence_active();
        bool thermal_sleep = (need_sleep &&
                              (sleep_owner == SLEEP_OWNER_THERMAL));

        if (thermal_sleep && !thermal_sleep_allowed())
        {
            if (driver_mode == MODE_ON)
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
            need_sleep = false;

        if (need_sleep || strobe_sleep)
        {
            adc_stop();
            tim2_stop();

            if (thermal_sleep)
            {
                /* Thermal sleep owns AWU; beacon/AUX must not overwrite the
                 * four-second sampling interval. */
                AWU_OFF();
                AWU_set_delay(AWU_4S);
                AWU_ON();
            }
            else if (!is_aux_mode(IDM_STROBE))
            {
                start_aux_beacon();
            }
            AWU_Sleep_Now();
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
             * write a regular brightness value during an input window. */
            if (!is_aux_mode(IDM_STROBE) &&
                (thermal_fast_required() ||
                 (driver_mode == MODE_ON && aux_is_active())) &&
                Released && !ss_is_active())
            {
                (void)sample_and_scan();
            }
        }

        if (tick_2hz())
        {
#ifdef WL_FLASH_DEBUG
            uint16_t out = (step_user > limiter) ? (step_user - limiter) : 0;
#endif
            EE_PRINTF("[NORMAL] T=%u F=%d I=%d L=%d P=%u S=%u V=%umV ramp=%u\r\n",
                      raw_temperature_adc, state.filtered_temp,
                      state.integral, state.prev_limiter, out, step_user,
                      vbat_to_mv_calibrated(), max_step);
        }

        if (tick_5s())
        {
            /* Deferred persistence: OB flushes only when its cache is dirty
             * (ob.c); EE writes only when the working copy differs from the
             * last-persisted shadow — an even number of toggles within the
             * window restores the old value and produces no flash write. */
            ob_Flush();
            if (memcmp(&ee_shadow, &ee, EE_STRUCT_SIZE))
            {
                need_save = 1;
                handle_ee();                      /* успех → need_save = 0 */
                if (!need_save)
                    memcpy(&ee_shadow, &ee, EE_STRUCT_SIZE);
                /* сбой: need_save остаётся 1 → ретрай на следующем тике */
            }
            else
            {
                need_save = 0;
            }
            if (driver_mode == MODE_ON)
            {
                if (thermal_sleep_allowed())
                {
                    /* Low-power scans are intentionally sparse.  Take one
                     * final fresh sample before stopping ADC/TIM2 so a stale
                     * low-temperature value cannot authorize standby. */
                    bool sample_ok = sample_and_scan();
                    if (sample_ok && thermal_sleep_allowed())
                    {
                        EE_PRINTF("[THERMAL SLEEP] S=%u L=%u\r\n",
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
                    /* High power, active limiting, soft-start, button
                     * activity, or AUX ownership keeps the MCU awake. */
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
    }
}
