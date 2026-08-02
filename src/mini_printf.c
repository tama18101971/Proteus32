#include "mini_printf.h"
#include "ch32v00x.h"

/**
 * @brief Initialise UART1 for debug output (bare-metal register writes).
 * @param baudrate Target baud rate (e.g. 460800).
 *
 * Configures PD5 as alternate-function push-pull output, computes the
 * baud-rate divisor from SYSCLK (48 MHz), and enables USART1 transmitter.
 * Uses direct register writes — not the SDK's USART_Printf_Init.
 */
void uart_init(uint32_t baudrate)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1;

    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xF << 20)) | (0xE << 20); // PD5 AF-PP 50MHz

    uint32_t div = (48000000u + (baudrate >> 1)) / baudrate;
    USART1->BRR = (uint16_t)(((div / 16) << 4) | (div % 16));

    USART1->CTLR1 = USART_CTLR1_UE | USART_CTLR1_TE;
}

/**
 * @brief Output a single character via USART1 (blocking).
 * @param c Character to send.
 */
static void out_char(char c)
{
    USART1->DATAR = (uint8_t)c;
    while (!(USART1->STATR & USART_STATR_TC));
}

/**
 * @brief Output a NUL-terminated string via USART1.
 * @param s String to send.
 */
static void out_string(const char *s)
{
    while (*s) out_char(*s++);
}

/**
 * @brief Output an unsigned integer in the given base.
 * @param val      Value to print.
 * @param base     Numeric base (10 or 16).
 * @param width    Minimum field width (padded with @p pad).
 * @param pad      Padding character.
 * @param uppercase Use uppercase hex digits if non-zero.
 */
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

/**
 * @brief Output a signed integer via USART1.
 * @param val Value to print (may be negative).
 */
static void out_signed(int32_t val)
{
    if (val < 0) {
        out_char('-');
        val = -val;
    }
    out_unsigned((uint32_t)val, 10, 0, '0', 0);
}

/**
 * @brief Core format-string parser and dispatcher.
 * @param fmt Format string (supports %d, %u, %x, %X, %s, %c, %%).
 * @param ap  Variable argument list.
 */
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

/**
 * @brief Minimal printf implementation (bare-metal, no stdlib).
 * @param fmt Format string (supports %d, %u, %x, %X, %s, %c, %%).
 * @param ... Variable arguments.
 * @return Always 0.
 */
int mini_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_format(fmt, ap);
    va_end(ap);
    return 0;
}
