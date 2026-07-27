/*
 * main.c — Точка входа и главный цикл проекта Proteus003
 *
 * Управление фонариком на базе CH32V003:
 * - 2 канала LED (красный, зелёный) через DAC
 * - NTC-датчик температуры (ADC)
 * - Кнопка управления (EXTI)
 * - AWU (Auto Wake-Up) для периодических задач и сна
 * - TIM2 (1мс системный тик) для таймерных интервалов
 * - EEPROM-эмуляция во Flash (настройки)
 *
 * Главный цикл:
 * 1. Если need_sleep — останавливаем ADC и TIM2, засыпаем в STANDBY
 * 2. При пробуждении по AWU — обрабатываем AUX-команды
 * 3. При пробуждении по кнопке — восстанавливаем периферию
 * 4. По таймерам: 150Гц (UI + softstart), 60Гц (ADC), 5с (EEPROM)
 */
#include <stdint.h>
#include <stdbool.h>
#include "led_gpio.h"
#include "i2c.h"
#include "dac.h"
#include "button_gpio.h"
#include "button.h"
#include "aux_leds.h"
#include "wl_flash.h"
#include "ui.h"
#include "adc.h"
#include "timers.h"
#include "limiters.h"
#include "ob.h"
#include "array.h"

volatile wakeup_source_t wakeup_source = WS_NONE; /* Источник последнего пробуждения */

uint8_t need_sleep = false; /* Флаг необходимости перейти в режим сна */

int16_t error; /* Текущая ошибка (filtered - setpoint) */

int main(void)
{

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    Delay_Ms(3000); /* Задержка перед инициализацией (стабилизация питания) */

#ifdef WL_FLASH_DEBUG
    USART_Printf_Init(460800);
#endif

    EE_PRINTF("\r\n[MAIN] Boot...\r\n");

    sys_init();

    AWU_init();              /* Включение PWR + LSI генератора */
    AWU_int_init();          /* Настройка EXTI Line9 + NVIC */
    AWU_set_delay(AWU_15MS); /* Начальная задержка AWU */

    led_init();    /* Инициализация GPIO для LED */
    button_gpio_init(); /* Инициализация EXTI для кнопки */

/*     while(1){
        on_grn_h();
        on_red_h();
        Delay_Ms(2000);
        off_grn();
        off_red();
        Delay_Ms(2000);
        on_grn_l();
        on_red_l();
        Delay_Ms(5000);
    }
 */
    adc_init(); /* Инициализация АЦП (NTC + Vrefint) */

    /* DAC + I2C */
    dac_pwrpin_init(); // настраиваем ногу для управления питанием.
    dac_pwrpin_on();   // включаем питание DAC
    i2c_init(400000);
    dac7571_set_voltage(0);

 //   if (read_ee() == FAILED) /* Загрузка настроек из EEPROM (или инициализация по умолчанию) */
 //   {
 //       init_ee();
 //   }

    ee_defaults(&ee); /* RAM-инициализация по умолчанию (без обращения к Flash).
                        * Единый источник правды для ee.slots[], default__ui1/2 и т.д.,
                        * см. src/ui.c. Ниже — только оверрайды для текущего тестового стенда. */

    ee.uif.off_bckl       = true;          // ee_defaults() не трогает это поле (остаётся false)
    ee.aux.bcklight_color = BCK_COLOR_GRN; // default: BCK_COLOR_RED
    ee.aux.beacon_mode    = BCON_4S;       // default: BCON_1S
    ee.aux.beacon_on      = false;         // default: true
    ee.aux.beacon_type    = 1;             // default: GRN_LED(2); тестовый стенд хочет RED_LED(1)
    ob_SetMl(5);                           // default: 1


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

    // start_aux_cmd(INFO_GRN, true, IDM_BEACON, AWU_120MS);
    // start_aux_array(buf_sos_red, AWU_120MS); /* Запуск тестового массива мигания */

    start_aux_beacon();

    need_sleep = true;

    /* Интервалы таймеров (аналог millis в Arduino) */

    reset_limiters(); // Сброс ограничителей

    /* Приветственное сообщение */
    //EE_PRINTF("\r\nthermal_limiter started\r\n");
    EE_PRINTF("setpoint=%u kp_shift=%u ki_shift=%u ki_decim=%u offset=%u\r\n",
           T_SET, config.kp_left_shift, config.ki_left_shift,
           config.ki_decimation_shift, config.t_anticipate_offset);



    while (1)
    {
        // wakeup_source_t ws = WS_NONE;

        /* 1. Сон: останавливаем ADC и TIM2, засыпаем в STANDBY */
        if (need_sleep)
        {
            // EE_PRINTF("[MAIN] Entering sleep mode...\r\n");
            adc_stop();
            tim2_stop();
            start_aux_beacon();
            AWU_Sleep_Now();
        }

        /* 2. Пробуждение по кнопке: восстанавливаем периферию */
        if ((wakeup_source == WS_BUTTON) && is_sleep_mode)
        {
            // EE_PRINTF("[MAIN] Event source: BUTTON.\r\n");
            is_sleep_mode = false;
            need_sleep = false;
            wakeup_source = WS_NONE;
            stop_aux_beacon(false);
            adc_init();
            tim2_start();
            dac_pwrpin_init();
            i2c_init(400000);
        }

        /* 3. Пробуждение по AWU: обрабатываем AUX-команды */
        if (wakeup_source == WS_AWU)
        {
            // EE_PRINTF("[MAIN] Event source: AWU.\r\n");
            wakeup_source = WS_NONE;
            handle_aux();
        }

        /* 4. Таймерные задачи (выполняются пока need_sleep == false) */
        if (tick_160hz()) /* ~6мс: UI (button FSM + dispatcher) + softstart */
        {
            // EE_PRINTF("[MAIN] Tick 160Hz.\r\n");
            button_tick();
            ui_process();
            ss_scan();
        }

        if (tick_30hz()) /* ~33мс: */
        {
            // EE_PRINTF("[MAIN] Tick 50Hz.\r\n");
            adc_read();
            scan_limiters();
        }

        if (tick_2hz()) /* 500мс: */
        {
            // EE_PRINTF("[MAIN] Tick 2Hz.\r\n");

            /* Формат: T=сырой F=t_cur(фильтр) I=интегратор L=limiter P=PWM */
            EE_PRINTF("T=%u F=%d I=%d L=%d P=%u\r\n",
                   raw_temperature_adc, state.filtered_temp,
                   state.integral, state.prev_limiter, step_user);

            /* Serial Plotter: Label:value, каждый на отдельной строке */
            //EE_PRINTF("F:%d\r\n", state.filtered_temp);
            //EE_PRINTF("S:%u\r\n", T_SET);
            //EE_PRINTF("L:%d\r\n", state.prev_limiter);
        }

        if (tick_5s()) /* 5с: сохранение EEPROM */
        {
            // EE_PRINTF("[MAIN] Tick 5 Sec.\r\n");
//            handle_ee();
            need_sleep = true;
        }
    }
}
