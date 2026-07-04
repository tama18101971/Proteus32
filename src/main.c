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
#include "aux_leds.h"
#include "wl_flash.h"
#include "ui.h"
#include "adc.h"
#include "timers.h"
#include "limiters.h"

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

    /* Приветственное сообщение */
    EE_PRINTF("\r\nthermal_limiter started\r\n");
    EE_PRINTF("setpoint=%d Kp=%d Ki=%d max=%d\r\n",
              config.temp_setpoint, config.Kp, config.Ki, config.max_limit);

    AWU_init();              /* Включение PWR + LSI генератора */
    AWU_int_init();          /* Настройка EXTI Line9 + NVIC */
    AWU_set_delay(AWU_15MS); /* Начальная задержка AWU */

    led_init();    /* Инициализация GPIO для LED */
    button_init(); /* Инициализация EXTI для кнопки */

    adc_init(); /* Инициализация АЦП (NTC + Vrefint) */

    /* DAC + I2C */
    dac_pwrpin_init(); // настраиваем ногу для управления питанием.
    dac_pwrpin_on();   // включаем питание DAC
    i2c_init(I2C_SPEED_400K);
    dac7571_set_voltage(0);

    if (read_ee() == FAILED) /* Загрузка настроек из EEPROM (или инициализация по умолчанию) */
    {
        init_ee();
    }

    // start_aux_cmd(INFO_GRN, true, IDM_BEACON, AWU_120MS);
    // start_aux_array(buf_sos_red, AWU_120MS); /* Запуск тестового массива мигания */

    start_aux_beacon();

    need_sleep = true;

    /* Интервалы таймеров (аналог millis в Arduino) */

    reset_limiters(); // Сброс ограничителей

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
            i2c_init(I2C_SPEED_400K);
        }

        /* 3. Пробуждение по AWU: обрабатываем AUX-команды */
        if (wakeup_source == WS_AWU)
        {
            // EE_PRINTF("[MAIN] Event source: AWU.\r\n");
            wakeup_source = WS_NONE;
            handle_aux();
        }

        /* 4. Таймерные задачи (выполняются пока need_sleep == false) */
        if (tick_160hz()) /* ~6мс: ADC измерение + UI + softstart */
        {
            // EE_PRINTF("[MAIN] Tick 160Hz.\r\n");
            scan_ui1();
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
            /* Вычислить ошибку для отладочного вывода */
            error = state.filtered_temp - (int16_t)config.temp_setpoint;

            /* Формат: T=сырой F=фильтр E=ошибка I=интегратор L=limiter P=P_term */
            EE_PRINTF("T=%d F=%d E=%d I=%ld L=%ld P=%ld\r\n",
                      raw_temperature_adc, state.filtered_temp, error,
                      (long)state.integral, (long)limiter, (long)state.p_term_out);
        }

        if (tick_5s()) /* 5с: сохранение EEPROM */
        {
            // EE_PRINTF("[MAIN] Tick 5 Sec.\r\n");
            handle_ee();
            // need_sleep = true;
        }
    }
}
