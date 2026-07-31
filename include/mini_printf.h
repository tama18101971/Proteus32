#ifndef MINI_PRINTF_H
#define MINI_PRINTF_H

#include <stdarg.h>
#include <stdint.h>

int mini_printf(const char *fmt, ...);
//void mini_vprintf(const char *fmt, va_list ap);

void uart_init(uint32_t baudrate);

#endif // MINI_PRINTF_H
