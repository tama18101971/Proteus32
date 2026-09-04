/* ============================================================
 * wl_flash.c — wear-levelling record ring over raw Flash pages
 *
 * Layout: WL_PAGE_COUNT pages of WL_FLASH_PAGE_SIZE bytes at the end of
 * Flash, each page holding WL_SLOTS_PER_PAGE records. Records are
 * appended slot by slot; when the cursor rotates onto the first slot of
 * a page, that page is erased and reused.
 *
 * Addressing goes through a two-word cursor — current page base and
 * current record address — advanced by wl_cursor_next(). Nothing here
 * multiplies or divides by the record size at run time: RV32EC has
 * neither a divider nor a multiplier, so a stray `%` drags in a
 * 128-byte libgcc helper. It also keeps the arithmetic honest when the
 * record size does not divide the page size evenly (8, 12, 20, 24, 28),
 * in which case each page ends with a few bytes the ring must never
 * address.
 * ============================================================ */
#include "wl_flash.h"

#include <string.h>

#include "wl_flash_hal.h"

#ifdef WL_FLASH_DEBUG
#include <stdio.h>
#define WL_TRACE(...) printf(__VA_ARGS__)
#else
#define WL_TRACE(...) ((void)0)
#endif

/* Bytes covered by the CRC: everything ahead of the crc field. */
#define WL_CRC_SPAN (WL_RECORD_SIZE - WL_CRC_SIZE)

/* Addressable bytes per page. Equals the page size only when the record
 * size divides it evenly; otherwise the remainder is dead space that the
 * ring skips. Compile-time constant, so no run-time multiply. */
#define WL_PAGE_USABLE (WL_SLOTS_PER_PAGE * WL_RECORD_SIZE)

/* ─── State ─────────────────────────────────────────────────────── */

static wl_record_t s_rec; /**< RAM working copy. */

/* Write cursor. s_page is the base of the page being filled; s_addr is the
 * slot that will be written next. `s_addr == s_page` means the cursor has
 * come back around to the first slot of the page, which is the point at
 * which that page has to be recycled. */
static uint32_t s_page;
static uint32_t s_addr;

/* Highest sequence number known to be stored in Flash, 0 when none.
 * Deliberately separate from the working copy: wl_init() resets the
 * payload but must not reset this, or a factory reset would write a
 * record that loses to the stale one it was meant to replace. */
static uint32_t s_seq;

/* False until the cursor has been positioned by a scan. */
static bool s_located;

/* Set from interrupt context, cleared before the write it triggers. */
static volatile uint8_t s_dirty;

static uint8_t s_retry;
static bool    s_busy;

/* ─── CRC ───────────────────────────────────────────────────────────
 * Both variants are the catalogued algorithms, so a host-side tool can
 * validate a Flash dump without reimplementing anything:
 *
 *   CRC-32              poly 0x04C11DB7 reflected (0xEDB88320), init
 *                       0xFFFFFFFF, final xor 0xFFFFFFFF.
 *                       check("123456789") == 0xCBF43926 — identical to
 *                       zlib.crc32 and Ethernet FCS.
 *   CRC-16/CCITT-FALSE  poly 0x1021, init 0xFFFF, no final xor.
 *                       check("123456789") == 0x29B1.
 *
 * Both init values are nonzero on purpose. With init 0 the CRC of an
 * all-zero record is 0, so the record validates itself and a wiped page
 * would masquerade as real data carrying sequence number 0.
 * ------------------------------------------------------------------ */

#if WL_CRC_BITS == 32
static uint32_t wl_crc(const void *data)
{
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0u; i < WL_CRC_SPAN; i++)
    {
        crc ^= buf[i];
        for (uint32_t bit = 0u; bit < 8u; bit++)
        {
            /* Branchless conditional xor. A multiply would be shorter to
             * write but RV32EC has no multiplier. */
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
#else
static uint16_t wl_crc(const void *data)
{
    const uint8_t *buf = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0u; i < WL_CRC_SPAN; i++)
    {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (uint32_t bit = 0u; bit < 8u; bit++)
        {
            crc = (uint16_t)((crc & 0x8000U) ? ((uint16_t)(crc << 1) ^ 0x1021U)
                                             : (uint16_t)(crc << 1));
        }
    }
    return crc;
}
#endif

/* ─── Cursor ────────────────────────────────────────────────────── */

static void wl_cursor_set(uint32_t page, uint32_t addr)
{
    s_page = page;
    s_addr = addr;
}

static void wl_cursor_next(void)
{
    s_addr += WL_RECORD_SIZE;

    if (s_addr >= s_page + WL_PAGE_USABLE)
    {
        s_page += WL_FLASH_PAGE_SIZE;
        if (s_page >= WL_REGION_END)
        {
            s_page = WL_REGION_START;
        }
        s_addr = s_page;
    }
}

/* ─── Scan ──────────────────────────────────────────────────────── */

typedef struct {
    uint32_t page;     /**< Page base holding the newest valid record. */
    uint32_t addr;     /**< Address of the newest valid record. */
    uint32_t seq;      /**< Its sequence number, 0 when none was found. */
    bool     found;    /**< A record passed its CRC. */
    bool     any_data; /**< At least one slot was not blank. */
} wl_scan_t;

/**
 * @brief  Walk every slot and locate the newest record with a valid CRC.
 *
 * Never touches the working copy: wl_commit() needs the cursor without
 * losing whatever the application has staged in RAM. Getting this wrong
 * is what made a stage-then-commit sequence silently write back stale
 * Flash contents in earlier versions of this library.
 *
 * The sequence number is read on its own first, and the full record plus
 * its CRC only when that number can still win. Blank and stale slots
 * therefore cost a 4-byte compare instead of a full CRC pass, which is
 * what dominates start-up time once the region is full.
 *
 * Sequence number 0 never wins: it is reserved for "nothing stored".
 * Among records sharing a sequence number the first in scan order wins,
 * so the result does not depend on scan direction.
 *
 * @param  out      Scan result.
 * @param  scratch  Caller-provided record buffer used for CRC checks.
 */
static void wl_scan(wl_scan_t *out, wl_record_t *scratch)
{
    uint32_t page = WL_REGION_START;

    out->page     = WL_REGION_START;
    out->addr     = WL_REGION_START;
    out->seq      = 0u;
    out->found    = false;
    out->any_data = false;

    for (uint32_t p = 0u; p < WL_PAGE_COUNT; p++)
    {
        uint32_t addr = page;
        uint32_t end  = page + WL_PAGE_USABLE;

        while (addr < end)
        {
            if (!wl_hal_is_erased(addr, WL_RECORD_SIZE))
            {
                uint32_t seq;

                out->any_data = true;
                wl_hal_read(addr, &seq, sizeof seq);

                if (seq > out->seq)
                {
                    wl_hal_read(addr, scratch, WL_RECORD_SIZE);
                    if (wl_crc(scratch) == scratch->crc)
                    {
                        out->page  = page;
                        out->addr  = addr;
                        out->seq   = seq;
                        out->found = true;
                    }
                }
            }
            addr += WL_RECORD_SIZE;
        }
        page += WL_FLASH_PAGE_SIZE;
    }
}

/**
 * @brief  Position the cursor and s_seq from the contents of Flash.
 * @param  out      Scan result, for callers that need the details.
 * @param  scratch  Caller-provided record buffer.
 */
static void wl_locate(wl_scan_t *out, wl_record_t *scratch)
{
    wl_scan(out, scratch);

    if (out->found)
    {
        s_seq = out->seq;
        wl_cursor_set(out->page, out->addr);
        wl_cursor_next();
    }
    else
    {
        /* Nothing selectable. Start at the top of the region; whatever
         * garbage sits there fails its CRC and is either skipped or
         * recycled as the ring comes around. */
        s_seq = 0u;
        wl_cursor_set(WL_REGION_START, WL_REGION_START);
    }

    s_located = true;
}

/* ─── Slot selection ────────────────────────────────────────────── */

/**
 * @brief  Move the cursor onto a slot that can be programmed.
 *
 * The first slot of a page means the ring has come all the way around,
 * so the page is recycled with an erase. Any other slot must already be
 * blank.
 *
 * A non-blank slot in mid-page is the fingerprint of an interrupted
 * write. Such a page is never erased: it still holds the newest
 * confirmed record, and erasing it would open a window where a reset
 * loses that record. Instead the cursor steps over the damaged slot and
 * keeps the remaining blank slots of the page usable, costing one slot
 * rather than a whole page plus an extra erase cycle.
 *
 * Terminates: each step moves the cursor forward, and at most
 * WL_SLOTS_PER_PAGE - 1 steps reach the first slot of the next page,
 * which returns unconditionally.
 *
 * @return WL_OK, or WL_HW_ERROR when the page erase fails.
 */
static wl_status_t wl_seek_writable(void)
{
    while (s_addr != s_page)
    {
        if (wl_hal_is_erased(s_addr, WL_RECORD_SIZE))
        {
            return WL_OK;
        }

        WL_TRACE("[wl] slot %08lx not blank, skipping\r\n",
                 (unsigned long)s_addr);
        wl_cursor_next();
    }

    if (wl_hal_is_erased(s_page, WL_FLASH_PAGE_SIZE))
    {
        return WL_OK;
    }

    if (!wl_hal_erase_page(s_page))
    {
        WL_TRACE("[wl] erase failed at %08lx\r\n", (unsigned long)s_page);
        return WL_HW_ERROR;
    }

    return WL_OK;
}

/* ─── Public API ────────────────────────────────────────────────── */

void wl_init(void)
{
    memset(&s_rec, 0, sizeof s_rec);
    s_rec.version = WL_FORMAT_VERSION;

    /* s_seq, the cursor and s_located survive on purpose — see s_seq. */
    s_dirty = 0u;
    s_retry = 0u;
}

wl_status_t wl_load(void)
{
    wl_record_t scratch;
    wl_scan_t   scan;

    wl_locate(&scan, &scratch);
    s_retry = 0u;

    if (!scan.found)
    {
        WL_TRACE("[wl] no valid record (%s)\r\n",
                 scan.any_data ? "corrupt" : "blank");
        return scan.any_data ? WL_CORRUPT : WL_EMPTY;
    }

    /* The winner is only still in scratch if it happened to be the last
     * candidate examined, so fetch it from its known address. */
    wl_hal_read(scan.addr, &scratch, WL_RECORD_SIZE);

    if (scratch.version != WL_FORMAT_VERSION)
    {
        WL_TRACE("[wl] version %u != %u\r\n", (unsigned)scratch.version,
                 (unsigned)WL_FORMAT_VERSION);
        return WL_BAD_VERSION;
    }

    memcpy(&s_rec, &scratch, WL_RECORD_SIZE);
    s_dirty = 0u;

    WL_TRACE("[wl] loaded seq=%lu from %08lx\r\n", (unsigned long)s_rec.seq,
             (unsigned long)scan.addr);
    return WL_OK;
}

wl_status_t wl_commit(void)
{
    wl_status_t st;

    /* Best-effort re-entrancy guard, not a lock: the test-and-set is not
     * atomic, so an interrupt landing between the two lines could still get
     * through. It catches the realistic case — an ISR calling wl_commit()
     * while the main loop is inside one — and turns it into a clean error
     * instead of a corrupted cursor. Interrupt handlers should call
     * wl_mark_dirty() and let the main loop do the writing. */
    if (s_busy)
    {
        return WL_BUSY;
    }
    s_busy = true;

    if (!s_located)
    {
        wl_record_t scratch;
        wl_scan_t   scan;
        wl_locate(&scan, &scratch);
    }

    /* Sequence numbers are the only thing ordering records, and 0 means
     * "nothing stored". Refuse to wrap rather than let older records win.
     * Unreachable in practice: Flash endurance runs out many orders of
     * magnitude before 2^32 writes. */
    if (s_seq == 0xFFFFFFFFUL)
    {
        s_busy = false;
        return WL_NO_SPACE;
    }

    st = wl_seek_writable();
    if (st != WL_OK)
    {
        s_retry++;
        s_busy = false;
        return st;
    }

    s_rec.seq     = s_seq + 1u;
    s_rec.version = WL_FORMAT_VERSION;
    s_rec.crc     = wl_crc(&s_rec);

    if (!wl_hal_program(s_addr, &s_rec, WL_RECORD_SIZE) ||
        !wl_hal_verify(s_addr, &s_rec, WL_RECORD_SIZE))
    {
        WL_TRACE("[wl] write failed at %08lx\r\n", (unsigned long)s_addr);
        /* Step over the damaged slot: retrying into a cell that is
         * already partially programmed can corrupt it further. */
        wl_cursor_next();
        s_retry++;
        s_busy = false;
        return WL_HW_ERROR;
    }

    WL_TRACE("[wl] wrote seq=%lu at %08lx\r\n", (unsigned long)s_rec.seq,
             (unsigned long)s_addr);

    s_seq = s_rec.seq;
    wl_cursor_next();
    s_retry = 0u;
    s_busy  = false;
    return WL_OK;
}

void wl_mark_dirty(void)
{
    s_dirty = 1u;
}

bool wl_is_dirty(void)
{
    return s_dirty != 0u;
}

wl_status_t wl_commit_if_dirty(void)
{
    wl_status_t st;

    if (s_dirty == 0u)
    {
        return WL_OK;
    }

    if (s_retry >= WL_MAX_COMMIT_RETRY)
    {
        return WL_RETRY_EXHAUSTED;
    }

    /* Clear before writing: a wl_mark_dirty() raised by an interrupt
     * while the write is in flight must not be swallowed afterwards. */
    s_dirty = 0u;

    st = wl_commit();
    if (st != WL_OK)
    {
        s_dirty = 1u;
    }
    return st;
}

void *wl_data(void)
{
    return s_rec.data;
}

uint32_t wl_seq(void)
{
    return s_rec.seq;
}

#ifdef WL_FLASH_TESTING
void wl_test_reset_state(void)
{
    memset(&s_rec, 0, sizeof s_rec);
    s_page    = 0u;
    s_addr    = 0u;
    s_seq     = 0u;
    s_located = false;
    s_dirty   = 0u;
    s_retry   = 0u;
    s_busy    = false;
}
#endif
