#include "mini_printf.h"
#include "ch32v00x.h"

static void out_char(char c)
{
    USART_SendData(USART1, (uint16_t)c);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET);
}

static void out_string(const char *s)
{
    while (*s) out_char(*s++);
}

static void out_unsigned(uint32_t val, uint8_t base, uint8_t width, char pad, uint8_t uppercase)
{
    char buf[11];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val) {
            buf[i++] = digits[val % base];
            val /= base;
        }
    }

    while (i < width) {
        out_char(pad);
        width--;
    }

    while (i > 0) {
        out_char(buf[--i]);
    }
}

static void out_signed(int32_t val)
{
    if (val < 0) {
        out_char('-');
        val = -val;
    }
    out_unsigned((uint32_t)val, 10, 0, '0', 0);
}

static void out_format(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            out_char(*fmt++);
            continue;
        }

        fmt++;
        char pad = ' ';
        uint8_t width = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        uint8_t long_flag = 0;
        if (*fmt == 'l') {
            long_flag = 1;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            if (long_flag)
                out_signed((int32_t)va_arg(ap, long));
            else
                out_signed(va_arg(ap, int));
            break;

        case 'u':
            if (long_flag)
                out_unsigned(va_arg(ap, unsigned long), 10, width, pad, 0);
            else
                out_unsigned(va_arg(ap, unsigned int), 10, width, pad, 0);
            break;

        case 'x':
            if (long_flag)
                out_unsigned(va_arg(ap, unsigned long), 16, width, pad, 0);
            else
                out_unsigned(va_arg(ap, unsigned int), 16, width, pad, 0);
            break;

        case 'X':
            if (long_flag)
                out_unsigned(va_arg(ap, unsigned long), 16, width, pad, 1);
            else
                out_unsigned(va_arg(ap, unsigned int), 16, width, pad, 1);
            break;

        case 's':
            out_string(va_arg(ap, const char *));
            break;

        case 'c':
            out_char((char)va_arg(ap, int));
            break;

        case '%':
            out_char('%');
            break;

        default:
            out_char('%');
            out_char(*fmt);
            break;
        }

        fmt++;
    }
}

int mini_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_format(fmt, ap);
    va_end(ap);
    return 0;
}
