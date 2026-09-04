#ifndef MINI_PRINTF_H
#define MINI_PRINTF_H

#include <stdarg.h>
#include <stdint.h>

/**
 * @brief Minimal printf implementation (bare-metal, no stdlib).
 * @param fmt Format string (supports %d, %u, %x, %X, %s, %c, %%).
 * @param ... Variable arguments.
 * @return Always 0.
 */
int mini_printf(const char *fmt, ...);
//void mini_vprintf(const char *fmt, va_list ap);

/**
 * @brief Initialise UART1 for debug output (bare-metal register writes).
 * @param baudrate Target baud rate (e.g. UART_DEBUG_BAUD).
 */
void uart_init(uint32_t baudrate);

#endif // MINI_PRINTF_H
