/* ============================================================
 * wl_flash_ob.c — Option Bytes store, policy layer
 *
 * Owns the RAM cache and the flush policy: dirty flag, the bounded
 * retry counter, and the status codes. Every Flash access goes through
 * the wl_hal_ob_* pair, which carries the whole destructive hardware
 * sequence — this file knows nothing about Flash registers.
 *
 * Compiles to nothing when WL_OB_ENABLE is 0.
 * ============================================================ */
#include "wl_flash_ob.h"

#if WL_OB_ENABLE

#include <string.h>

#include "wl_flash_hal.h"

/* ─── Module state ──────────────────────────────────────────────── */

static uint8_t s_cache[WL_OB_USER_BYTES];

/* Set from interrupt context, cleared before the write it triggers. */
static volatile bool s_dirty;

static uint8_t s_fails;

/* ─── Public API ────────────────────────────────────────────────── */

void wl_ob_init(void)
{
    wl_hal_ob_read(s_cache, (uint32_t)WL_OB_USER_BYTES);
    s_dirty = false;
    s_fails = 0u;
}

void *wl_ob_data(void)
{
    return s_cache;
}

bool wl_ob_is_dirty(void)
{
    return s_dirty;
}

void wl_ob_mark_dirty(void)
{
    s_dirty = true;
    s_fails = 0u; /* a fresh change restarts the retry budget */
}

wl_ob_status_t wl_ob_flush(void)
{
    uint8_t stored[WL_OB_USER_BYTES];

    if (!s_dirty)
    {
        return WL_OB_IDLE;
    }

    if (s_fails >= (uint8_t)WL_OB_MAX_FLUSH_RETRY)
    {
        /* Give up: stop erase-cycling an unusable region. Only a new
         * wl_ob_mark_dirty() — that is, new data — reopens the budget. */
        return WL_OB_FAIL;
    }

    /* Clear before writing: a wl_ob_mark_dirty() raised by an interrupt
     * while the write is in flight must not be swallowed afterwards. */
    s_dirty = false;

    /* A dirty flag without a real change costs no erase cycle: the area
     * already holds exactly these bytes. */
    wl_hal_ob_read(stored, (uint32_t)WL_OB_USER_BYTES);
    if (memcmp(stored, s_cache, WL_OB_USER_BYTES) == 0)
    {
        s_fails = 0u;
        return WL_OB_OK;
    }

    if (wl_hal_ob_program(s_cache, (uint32_t)WL_OB_USER_BYTES))
    {
        s_fails = 0u;
        return WL_OB_OK;
    }

    s_dirty = true; /* still pending */
    s_fails++;
    return WL_OB_FAIL;
}

/* ─── Test hook ─────────────────────────────────────────────────── */

#ifdef WL_FLASH_TESTING
void wl_ob_test_reset_state(void)
{
    memset(s_cache, 0, sizeof s_cache);
    s_dirty = false;
    s_fails = 0u;
}
#endif /* WL_FLASH_TESTING */

#else /* !WL_OB_ENABLE */

/* An empty translation unit is not valid C99. */
typedef int wl_flash_ob_disabled_t;

#endif /* WL_OB_ENABLE */
