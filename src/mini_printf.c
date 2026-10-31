// SPDX-License-Identifier: GPL-3.0-or-later

#include "mini_printf.h"
#include "ch32v00x.h"
#include "proteus_config.h"

/* Busy-wait iteration limit for USART1 flag polling: ~1 ms, derived from
 * SYSCLK_HZ.  Enough for one full byte at UART_DEBUG_BAUD (~22 us at
 * 460800) with generous margin, but short enough to avoid an observable
 * hang if USART1 is glitched or unclocked. */
#define USART_WAIT_LIMIT  UART_TC_WAIT_LOOPS

/* USART1->BRR кодирует USARTDIV*16 полями DIV_Mantissa[15:4] и
 * DIV_Fraction[3:0], поэтому целое частное SYSCLK/baud записывается в
 * регистр напрямую (с симметричным округлением). */
#define UART_BRR(baud)    ((SYSCLK_HZ + ((baud) / 2u)) / (baud))

/* Мантисса должна быть хотя бы 1: BRR < 16 означает делитель < 1. */
_Static_assert(UART_BRR(UART_DEBUG_BAUD) >= 16u,
               "UART_DEBUG_BAUD too high for SYSCLK_HZ (BRR mantissa is zero)");
/* Ошибка округления делителя должна оставаться существенно меньше
 * разброса HSI (+-2.2 % при -40..85 C, DS табл. 3-11), иначе суммарный
 * бюджет 8N1 исчерпан. Порог 2 % от номинала.
 * Текст сообщений — только ASCII: компилятор печатает его в консоль,
 * кодовая страница которой не обязана быть UTF-8. */
_Static_assert(SYSCLK_HZ / UART_BRR(UART_DEBUG_BAUD)
                   <= UART_DEBUG_BAUD + (UART_DEBUG_BAUD / 50u),
               "UART baud rate error above +2% - lower UART_DEBUG_BAUD");
_Static_assert(SYSCLK_HZ / UART_BRR(UART_DEBUG_BAUD)
                   >= UART_DEBUG_BAUD - (UART_DEBUG_BAUD / 50u),
               "UART baud rate error below -2% - lower UART_DEBUG_BAUD");

/**
 * @brief Initialise UART1 for debug output (bare-metal register writes).
 * @param baudrate Target baud rate (e.g. UART_DEBUG_BAUD).
 *
 * Configures PD5 as alternate-function push-pull output, computes the
 * baud-rate divisor from SYSCLK_HZ, and enables USART1 transmitter.
 * Uses direct register writes — not the SDK's USART_Printf_Init.
 */
void uart_init(uint32_t baudrate)
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1;

    /* PD5: AF push-pull 50 MHz. Код 0xE давал AF Open-Drain 2 MHz
     * (CNF=11b, MODE=10b) — фронт 460800 бод на OD без внешней подтяжки
     * завален; 0xB = CNF=10b (AF-PP), MODE=11b (50 MHz). */
    GPIOD->CFGLR = (GPIOD->CFGLR & ~(0xFu << 20)) | (0xBu << 20);

    uint32_t div = (SYSCLK_HZ + (baudrate >> 1)) / baudrate;
    USART1->BRR = (uint16_t)div;

    USART1->CTLR1 = USART_CTLR1_UE | USART_CTLR1_TE;
}

/**
 * @brief Queue one character in USART1's transmit data register.
 * @param c Character to send.
 * @details Waits for TXE with a bounded iteration count so a glitched or
 *          unclocked USART1 cannot freeze the main loop.  If the limit
 *          expires the character is silently dropped.
 */
static void out_char(char c)
{
    uint32_t wait = USART_WAIT_LIMIT;
    while (!(USART1->STATR & USART_STATR_TXE))
        if (!--wait) return;

    USART1->DATAR = (uint8_t)c;
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
 * @brief Output an unsigned integer in base 10 or 16.
 * @param val      Value to print.
 * @param base     Numeric base (10 or 16).
 * @param width    Minimum field width (padded with @p pad).
 * @param pad      Padding character.
 */
static void out_unsigned(uint32_t val, uint8_t base, uint8_t width, char pad)
{
    char buf[11];
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else if (base == 16) {
        while (val) {
            uint8_t d = (uint8_t)(val & 0x0F);
            buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            val >>= 4;
        }
    } else {
        while (val) {
            /* Точное целочисленное деление на 10 сдвигово-аддитивной
             * цепочкой (Hacker's Delight), без 32-битного умножения:
             * прежний магический вариант (val * 52429u) >> 19 переполнял
             * uint32_t при val >= 81920 и печатал мусорные разряды.
             * Корректность цепочки подтверждена хост-тестом на полном
             * диапазоне границ uint32_t. */
            uint32_t q = (val >> 1) + (val >> 2);
            q += q >> 4;
            q += q >> 8;
            q += q >> 16;
            q >>= 3;
            uint8_t rem = (uint8_t)(val - ((q << 3) + (q << 1)));
            if (rem > 9u) {
                rem = (uint8_t)(rem - 10u);
                q++;
            }
            buf[i++] = (char)('0' + rem);
            val = q;
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
    uint32_t mag = (uint32_t)val;
    if (val < 0) {
        out_char('-');
        mag = 0u - mag; /* без UB и для INT32_MIN */
    }
    out_unsigned(mag, 10, 0, '0');
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

        if (*fmt == 'l') {
            fmt++;
        }

        if (*fmt == '\0') {
            /* Lone '%' (optionally with flags/width) at the very end of
             * the format string: print it literally and stop.  Falling
             * into default would emit a stray NUL byte and walk fmt past
             * the terminator, scanning arbitrary flash for another '\0'. */
            out_char('%');
            break;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            out_signed(va_arg(ap, int32_t));
            break;

        case 'u':
            out_unsigned(va_arg(ap, uint32_t), 10, width, pad);
            break;

        case 'x':
        case 'X':
            out_unsigned(va_arg(ap, uint32_t), 16, width, pad);
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

    /* Preserve the existing blocking-message contract: callers return only
     * after the final byte has physically left USART1.
     * Bounded wait: if USART1 is glitched or unclocked, bail out. */
    uint32_t wait = USART_WAIT_LIMIT;
    while (!(USART1->STATR & USART_STATR_TC))
        if (!--wait) break;

    return 0;
}
