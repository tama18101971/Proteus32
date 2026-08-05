/*
    div32.c — компактные 32-битные деление/остаток вместо libgcc div.o

    Алгоритм — восстановительное деление (shift-subtract).
    Беззнаковые функции корректны для всех делителей, кроме нуля.
    Деление на ноль возвращает 0 как защитное поведение.
*/

#include <stdint.h>

/**
@brief Unsigned 32/32 division (replaces libgcc __udivsi3).
@param a Dividend.
@param b Divisor. 0 returns 0.
@return floor(a / b).
*/
uint32_t __udivsi3(uint32_t a, uint32_t b)
{
    uint32_t q = 0u;
    uint32_t r = 0u;
    int16_t i;

    if (b == 0u)
        return 0u;

    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((a >> (uint32_t)i) & 1u);

        if (r >= b) {
            r -= b;
            q |= UINT32_C(1) << (uint32_t)i;
        }
    }

    return q;
}

/**
@brief Unsigned 32/32 remainder (replaces libgcc __umodsi3).
@param a Dividend.
@param b Divisor. 0 returns 0.
@return a % b.
*/
uint32_t __umodsi3(uint32_t a, uint32_t b)
{
    if (b == 0u)
        return 0u;

    return a - __udivsi3(a, b) * b;
}

/**
@brief Signed 32/32 division (replaces libgcc __divsi3).
@param a Dividend.
@param b Divisor. 0 returns 0.
@return a / b, truncated toward zero.

INT32_MIN / -1 is C undefined behavior; here INT32_MIN is returned
as the common two's-complement saturated/truncated result.
*/
int32_t __divsi3(int32_t a, int32_t b)
{
    if (b == 0)
        return 0;

    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;

    uint32_t neg = (ua ^ ub) >> 31;

    if (ua >> 31)
        ua = 0u - ua;

    if (ub >> 31)
        ub = 0u - ub;

    uint32_t q = __udivsi3(ua, ub);

    /*
       Единственный случай q > INT32_MAX:
       |INT32_MIN| / 1.

       Также сюда попадает INT32_MIN / -1, который является
       overflow. Возвращаем INT32_MIN, как типично делают
       на two's-complement платформах.
    */
    if (q == UINT32_C(0x80000000))
        return INT32_MIN;

    return neg ? -(int32_t)q : (int32_t)q;
}

/**
@brief Signed 32/32 remainder (replaces libgcc __modsi3).
@param a Dividend.
@param b Divisor. 0 returns 0.
@return a % b, sign follows dividend.
*/
int32_t __modsi3(int32_t a, int32_t b)
{
    if (b == 0)
        return 0;

    uint32_t ua = (uint32_t)a;
    uint32_t ub = (uint32_t)b;

    uint32_t neg = ua >> 31;

    if (ua >> 31)
        ua = 0u - ua;

    if (ub >> 31)
        ub = 0u - ub;

    uint32_t r = __umodsi3(ua, ub);

    return neg ? -(int32_t)r : (int32_t)r;
}