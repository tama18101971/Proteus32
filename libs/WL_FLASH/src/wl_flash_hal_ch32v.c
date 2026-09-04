/* ============================================================
 * wl_flash_hal_ch32v.c — CH32V00x Flash back end for WL_FLASH
 *
 * Programs half-words through the on-chip Flash controller and erases
 * 64-byte pages with "fast page erase" (FLASH_CTLR_PAGE_ER), which
 * touches exactly one page. The 1 KB FLASH_CTLR_PER erase would wipe
 * every EEPROM page plus adjacent program Flash and defeat the whole
 * point of wear levelling.
 *
 * Interrupts are masked around each individual half-word program and
 * around the page erase, not around a whole record. The core stalls for
 * the duration of a Flash write anyway, so masking longer than one
 * hardware operation only adds interrupt latency without buying safety.
 *
 * Compiles to nothing when WL_FLASH_HAL_MOCK is defined, which lets the
 * host test suite substitute a RAM-backed model.
 * ============================================================ */
#include "wl_flash_config.h"

#ifndef WL_FLASH_HAL_MOCK

#include <string.h>

#include "ch32v00x.h"
#include "wl_flash.h"
#include "wl_flash_hal.h"

/* Flash controller unlock keys. Present in the vendor ch32v00x_flash.c
 * but not exported by any header, so they are restated here under a
 * library-private name. */
#define WL_KEY1 0x45670123UL
#define WL_KEY2 0xCDEF89ABUL

/* "Fast page erase" — 64 bytes. Provided by newer ch32v00x.h revisions. */
#ifndef FLASH_CTLR_PAGE_ER
#define FLASH_CTLR_PAGE_ER 0x00020000UL
#endif

/* ─── Interrupt masking ─────────────────────────────────────────── */

/* csrrci clears MIE (mstatus bit 3) and returns the previous mstatus.
 * Only that one bit is restored: the vendor __enable_irq() writes
 * mstatus bits 3 and 7 together and would also raise MPIE, which the
 * caller never asked for. The "memory" clobber keeps the compiler from
 * moving loads and stores across the barrier. */

static inline uint32_t wl_irq_save(void)
{
    uint32_t state;
    __asm volatile("csrrci %0, mstatus, 8" : "=r"(state)::"memory");
    return state & 0x08u;
}

static inline void wl_irq_restore(uint32_t state)
{
    if (state != 0u)
    {
        __asm volatile("csrsi mstatus, 8" ::: "memory");
    }
}

/* ─── Controller helpers ────────────────────────────────────────── */

/* Spin until the controller goes idle. The counter is a plain local on
 * purpose: the loop condition reads a volatile register, so it cannot be
 * optimised away, and keeping the counter in a register costs nothing. */
static bool wl_wait_idle(void)
{
    uint32_t timeout = WL_FLASH_TIMEOUT;

    while ((FLASH->STATR & FLASH_FLAG_BSY) != 0u)
    {
        if (--timeout == 0u)
        {
            return false; /* controller stuck */
        }
    }
    return true;
}

/* EOP and WRPRTERR are write-1-to-clear in STATR. */
static inline void wl_clear_flags(void)
{
    FLASH->STATR = (uint32_t)FLASH_FLAG_EOP | (uint32_t)FLASH_FLAG_WRPRTERR;
}

static bool wl_took_error(void)
{
    if ((FLASH->STATR & FLASH_FLAG_WRPRTERR) != 0u)
    {
        wl_clear_flags(); /* leaving it set would wedge the controller */
        return true;
    }
    return false;
}

static inline void wl_unlock(void)
{
    FLASH->KEYR = WL_KEY1;
    FLASH->KEYR = WL_KEY2;
}

static inline void wl_lock(void)
{
    FLASH->CTLR |= (uint32_t)FLASH_CTLR_LOCK;
}

/* Fast program/erase mode has its own key register and its own lock bit
 * (FLOCK). FLASH_CTLR_LOCK does not re-lock it. */
static inline void wl_unlock_fast(void)
{
    FLASH->MODEKEYR = WL_KEY1;
    FLASH->MODEKEYR = WL_KEY2;
}

static inline void wl_lock_fast(void)
{
    FLASH->CTLR |= (uint32_t)FLASH_CTLR_FLOCK;
}

/* Last line of defence for the two destructive operations. The region
 * bounds are checked at compile time, so this can only fire if the
 * address itself is wrong — a corrupted cursor, a wild pointer, a stack
 * overflow. On a part with 2 KB of RAM and 256 bytes of stack that is
 * worth 20 bytes of Flash: without it, a single bad word in RAM turns
 * into "erase an arbitrary Flash page", including live program code.
 * The vendor FLASH_ErasePage_Fast() has the same guard; this back end
 * drives the controller directly and would otherwise lose it. */
static inline bool wl_in_region(uint32_t addr, uint32_t len)
{
    return addr >= (uint32_t)WL_REGION_START &&
           len <= (uint32_t)WL_REGION_SIZE &&
           addr <= (uint32_t)WL_REGION_END - len;
}

/* ─── HAL entry points ──────────────────────────────────────────── */

bool wl_hal_is_erased(uint32_t addr, uint32_t len)
{
    const volatile uint32_t *word = (const volatile uint32_t *)addr;
    uint32_t count = len / 4u;

    for (uint32_t i = 0u; i < count; i++)
    {
        if (word[i] != 0xFFFFFFFFUL)
        {
            return false;
        }
    }
    return true;
}

void wl_hal_read(uint32_t addr, void *dst, uint32_t len)
{
    memcpy(dst, (const void *)addr, len);
}

bool wl_hal_verify(uint32_t addr, const void *src, uint32_t len)
{
    return memcmp((const void *)addr, src, len) == 0;
}

bool wl_hal_program(uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)src;
    bool ok = true;

    if (!wl_in_region(addr, len))
    {
        return false;
    }

    for (uint32_t off = 0u; off < len; off += 2u)
    {
        volatile uint16_t *dst = (volatile uint16_t *)(addr + off);
        uint16_t half;
        uint32_t irq;

        /* memcpy rather than a uint16_t* cast: the source is a record
         * struct, and aliasing it through a different type is undefined
         * even where the alignment works out. GCC folds this into a
         * single half-word load. */
        memcpy(&half, bytes + off, sizeof half);

        /* Unlock, program, re-lock — once per half-word, all inside the
         * masked window. Holding the unlock across the whole record would
         * be fewer register writes, but it would also leave the Flash
         * controller open to a stray write for the entire duration. */
        irq = wl_irq_save();

        wl_unlock();
        ok = wl_wait_idle();

        if (ok)
        {
            wl_clear_flags();

            /* Programming a cell that is not 0xFFFF can corrupt it on
             * this part, so a non-blank target is an error, not a retry. */
            if (*dst != 0xFFFFU)
            {
                ok = false;
            }
            else
            {
                FLASH->CTLR |= (uint32_t)FLASH_CTLR_PG;
                *dst = half;
                ok = wl_wait_idle();
                FLASH->CTLR &= ~(uint32_t)FLASH_CTLR_PG;

                if (ok && wl_took_error())
                {
                    ok = false;
                }
            }
        }

        wl_lock();
        wl_irq_restore(irq);

        if (!ok)
        {
            break;
        }
    }

    return ok;
}

bool wl_hal_erase_page(uint32_t addr)
{
    uint32_t irq;
    bool ok;

    if (!wl_in_region(addr, (uint32_t)WL_FLASH_PAGE_SIZE))
    {
        return false;
    }

    wl_unlock();
    wl_unlock_fast();

    irq = wl_irq_save();

    ok = wl_wait_idle();
    if (ok)
    {
        wl_clear_flags();

        FLASH->CTLR |= (uint32_t)FLASH_CTLR_PAGE_ER;
        FLASH->ADDR = addr;
        FLASH->CTLR |= (uint32_t)FLASH_CTLR_STRT;

        ok = wl_wait_idle();
        FLASH->CTLR &= ~(uint32_t)FLASH_CTLR_PAGE_ER;

        if (ok && wl_took_error())
        {
            ok = false;
        }
    }

    wl_irq_restore(irq);

    wl_lock_fast();
    wl_lock();
    return ok;
}

#else /* WL_FLASH_HAL_MOCK */

/* An empty translation unit is not valid C99. */
typedef int wl_hal_ch32v_disabled_t;

#endif /* WL_FLASH_HAL_MOCK */
