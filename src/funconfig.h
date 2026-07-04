#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

/*
 * funconfig.h — Конфигурация фреймворка ch32v003fun
 *
 * Этот файл ДОЛЖЕН находиться в корне проекта (рядом с platformio.ini).
 * ch32fun.h подключает его автоматически (#include "funconfig.h").
 *
 * Определяет:
 *   - Какой отладочный интерфейс использовать (UART или semihosting)
 *   - Скорость UART
 *   - Другие параметры фреймворка
 *
 * Полный список опций: ch32fun/ch32fun.h (раздел "Fun Configs")
 */

/*
 * FUNCONF_USE_DEBUGPRINTF — использовать semihosting (через отладчик)
 *   1 = printf выводится через отладчик (WCH-Link / minichlink)
 *   0 = printf НЕ используется через semihosting
 *   По умолчанию: 1 (если не задан FUNCONF_USE_UARTPRINTF)
 */
#define FUNCONF_USE_DEBUGPRINTF 0

/*
 * FUNCONF_USE_UARTPRINTF — использовать UART для printf
 *   1 = printf выводится на UART (PD5, 115200 8N1)
 *   0 = printf не используется через UART
 *   При обоих = 0: printf никуда не выводится
 */
#define FUNCONF_USE_UARTPRINTF  1

/*
 * FUNCONF_UART_PRINTF_BAUD — скорость UART для printf
 *   Должна совпадать с настройкой мониторинга в platformio.ini.
 *   Значение: 115200 бод (стандартная скорость)
 *   Диапазон: 9600 - 921600 (типично 115200)
 */
//#define FUNCONF_UART_PRINTF_BAUD 115200

#endif