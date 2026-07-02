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
#include "pi_reg.h"

volatile wakeup_source_t wakeup_source = WS_NONE; /* Источник последнего пробуждения */

extern uint8_t is_sleep_mode; /* Флаг режима сна: 0 — работает, 1 — в STANDBY */
extern uint8_t need_save; /* Флаг необходимости сохранения EEPROM */

uint8_t need_sleep = false; /* Флаг необходимости перейти в режим сна */
uint16_t raw_temperature_adc = 271; // 25°C


/* ============ СТРУКТУРЫ РЕГУЛЯТОРА ============ */

static pi_reg_config_t led_thermal_config;
static pi_reg_state_t  led_thermal_state;

static void sys_init(void) {
    // 1. Настройка конфигурации термоконтроля
    led_thermal_config.setpoint     = 375;  // Целевой код АЦП (~55°C)
    led_thermal_config.start_offset = 30;   // Начать упреждение за 15 кодов до целевой температуры

    // Коэффициенты масштабированы на 256 (например, Kp = 2.0 -> 2.0 * 256 = 512)
    led_thermal_config.kp           = 512;  // Kp = 2.0
    led_thermal_config.ki           = 64;   // Ki = 0.25
    led_thermal_config.k_predict    = 1024; // Kpredict = 4.0

    led_thermal_config.max_i_term   = 200;
    led_thermal_config.minimum_pwm  = 170;
    led_thermal_config.filter_shift = 3;    // Фильтр EMA температуры по 8 выборкам

    // Асимметричный шаг: гасим яркость быстро (по 3 единицы), возвращаем медленно (по 1 единице)
    led_thermal_config.slew_attack  = 5;
    led_thermal_config.slew_release = 2;
    led_thermal_config.emergency_threshold = 30;

    // 2. Считываем стартовое значение АЦП
    uint16_t initial_adc = raw_temperature_adc;

    // 3. Инициализируем структуру состояния регулятора
    pi_reg_init(&led_thermal_state, &led_thermal_config, initial_adc);
}


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

    AWU_init();         /* Включение PWR + LSI генератора */
    AWU_int_init();     /* Настройка EXTI Line9 + NVIC */
    AWU_set_delay(AWU_15MS); /* Начальная задержка AWU */

    led_init();         /* Инициализация GPIO для LED */
    button_init();      /* Инициализация EXTI для кнопки */

    adc_init();         /* Инициализация АЦП (NTC + Vrefint) */

    /* DAC + I2C */
    dac_pwrpin_init();  // настраиваем ногу для управления питанием.
    dac_pwrpin_on();    // включаем питание DAC
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

    while (1)
    {
        //wakeup_source_t ws = WS_NONE;

        /* 1. Сон: останавливаем ADC и TIM2, засыпаем в STANDBY */
        if (need_sleep)
        {
            //EE_PRINTF("[MAIN] Entering sleep mode...\r\n");
            adc_stop();
            tim2_stop();
            start_aux_beacon();
            AWU_Sleep_Now();
        }

        /* 2. Пробуждение по кнопке: восстанавливаем периферию */
        if ((wakeup_source == WS_BUTTON) && is_sleep_mode)
        {
            //EE_PRINTF("[MAIN] Event source: BUTTON.\r\n");
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
        if (tick_160hz())    /* ~6мс: ADC измерение + UI + softstart */
        {
            //EE_PRINTF("[MAIN] Tick 160Hz.\r\n");
            adc_read();
            scan_ui1();
            ss_scan();
        }

        if (tick_50hz())     /* ~20мс: */
        {
            //EE_PRINTF("[MAIN] Tick 50Hz.\r\n");
            uint16_t user_desired_pwm = 512;  // Максимальная мощность (без энкодера)

            // Вычисляем необходимую величину ограничения мощности
            uint16_t limiter = pi_reg_compute(&led_thermal_config, &led_thermal_state,
                                            raw_temperature_adc, user_desired_pwm);
        }

        if (tick_5s())       /* 5с: сохранение EEPROM */
        {
            //EE_PRINTF("[MAIN] Tick 5 Sec.\r\n");
            handle_ee();
            need_sleep = true;
        }
    }
}
