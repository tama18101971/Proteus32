/*
 * div32.c — Компактные 32-битные деление/остаток вместо libgcc div.o
 *
 * Заменяют целиком libgcc div.o (~126 B, включая неиспользуемые проектом
 * знаковые __divsi3/__modsi3).  Определения здесь (а не в libgcc) позволяют
 * LTO инлайнить и специализировать деления из проекта (константные
 * делители I2C/калибровок -> сдвиги); внешние объекты (SDK
 * SystemCoreClockUpdate, библиотека I2C) линкуются на эти же символы.
 *
 * Алгоритм — восстановительное деление (shift-subtract), без умножения
 * и таблиц.  Реализация точна: q = a/b, r = a%b, при делителе b < 2^31
 * (все реальные делители: ~48 МГц/800 кГц/<=60/<=7).
 */

#include <stdint.h>

/**
 * @brief Unsigned 32/32 division (replaces libgcc __udivsi3).
 * @param a Dividend (0..UINT32_MAX).
 * @param b Divisor, must be < 2^31 (documented constraint — shift of the
 *        32-bit remainder cannot overflow). 0 returns 0 (safety, libgcc UB).
 * @return floor(a/b).
 */
uint32_t __udivsi3(uint32_t a, uint32_t b)
{
    uint32_t q = 0u;
    uint32_t r = 0u;
    int16_t i;

    if (b == 0u)
        return 0u;

    for (i = 31; i >= 0; i--)
    {
        r = (r << 1) | ((a >> (uint32_t)i) & 1u);
        if (r >= b)
        {
            r -= b;
            q |= (1u << (uint32_t)i);
        }
    }
    return q;
}

/**
 * @brief Unsigned 32/32 remainder (replaces libgcc __umodsi3).
 * @param a Dividend.
 * @param b Divisor, must be < 2^31. 0 returns 0 (safety).
 * @return a % b.
 */
uint32_t __umodsi3(uint32_t a, uint32_t b)
{
    if (b == 0u)
        return 0u;
    return a - __udivsi3(a, b) * b;
}

/**
 * @brief Signed 32/32 division (replaces libgcc __divsi3).
 * Not used by the project — kept for completeness in case the SDK or the
 * I2C library ever references it. LTO drops it if unreferenced.
 * @return a / b (C99, quotient truncated toward zero).
 */
int32_t __divsi3(int32_t a, int32_t b)
{
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    uint32_t q;

    if (b == 0)
        return 0;
    if (ua & 0x80000000u)
        ua = 0u - ua;                 /* беззнаковое отрицание — без UB */
    if (ub & 0x80000000u)
        ub = 0u - ub;

    q = __udivsi3(ua, ub);
    return ((int32_t)a < 0) == ((int32_t)b < 0) ? (int32_t)q : -(int32_t)q;
}

/**
 * @brief Signed 32/32 remainder (replaces libgcc __modsi3).
 * Kept for completeness like __divsi3; result sign follows the dividend.
 * @return a % b.
 */
int32_t __modsi3(int32_t a, int32_t b)
{
    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;
    uint32_t r;

    if (b == 0)
        return 0;
    if (ua & 0x80000000u)
        ua = 0u - ua;
    if (ub & 0x80000000u)
        ub = 0u - ub;

    r = __umodsi3(ua, ub);
    return ((int32_t)a < 0) ? -(int32_t)r : (int32_t)r;
}
