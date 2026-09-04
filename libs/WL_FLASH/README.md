# WL_FLASH — wear-levelling record store for WCH CH32V003

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Keeps a small configuration record in on-chip Flash on the CH32V003, which has
no EEPROM. Records are appended to a ring of 64-byte pages, each carrying a
sequence number and a CRC; the newest record that passes both checks wins at
boot. Erase wear spreads across every page in the region, and an interrupted
write cannot destroy the previous record.

Costs about **910 bytes of Flash and 36 bytes of RAM** in the default
configuration. No dynamic allocation, no RTOS, no dependency on the vendor
`debug.h`.

## What it guarantees

- **A record is only selected when its CRC and format version check out.**
  Power loss during a write leaves a partial record whose CRC fails, so the
  previous record stays current.
- **A slot is only written when every one of its words reads as erased.** A
  half-programmed record can never be mistaken for free space.
- **The page holding the newest confirmed record is never erased.** A reset
  during recovery cannot roll data back.
- **Every write is read back and compared.** A controller that reports success
  while storing wrong data is caught immediately, not at the next boot.
- **Wear spreads evenly.** One page erase is amortised over `WL_SLOTS_PER_PAGE`
  writes — 8 of them in the most compact configuration.

What it does *not* do: encrypt or authenticate anything. Records sit in Flash
as plain text, readable over SWD unless you set the read-protection option
byte. A CRC detects accidental corruption; it is not a MAC and will not stop
deliberate tampering. `wl_load() == WL_OK` means "these bytes are intact", not
"these bytes are trustworthy".

## Install

```ini
lib_deps =
    https://github.com/tama18101971/WL_FLASH.git#v2.0.1

; Required: hold the EEPROM region back from the linker.
; <flash size> - WL_PAGE_COUNT * 64. For a 16 KB part with the default
; 4 pages: 16384 - 256 = 16128.
board_upload.maximum_size = 16128
```

That second setting is not optional and not something the library can do for
you — PlatformIO does not evaluate arithmetic in `platformio.ini`, so the value
has to be a literal. Get it wrong and the linker will place code inside the
record region, where the first commit erases it. The library ships a build hook
that recomputes the number and fails the build with the right one:

```
WL_FLASH: the EEPROM region is not excluded from the firmware image.

  Flash size           : 16384 bytes
  EEPROM region        : 512 bytes (WL_PAGE_COUNT=8 x 64)
  Region address range : 0x08003E00..0x08004000
  maximum_size is      : 16128 bytes
  maximum_size must be : 15872 bytes
```

## Usage

```c
#include "wl_flash.h"

/* Whatever needs to survive a power cycle. */
typedef struct {
    uint8_t brightness;
    int8_t  trim;
    uint8_t mode;
} my_cfg_t;

WL_ASSERT_PAYLOAD_FITS(my_cfg_t);   /* build error if it does not fit */
#define CFG WL_DATA_AS(my_cfg_t)

int main(void)
{
    if (wl_load() != WL_OK)
    {
        /* Blank chip, corrupted region, or a record from an older format.
         * wl_load() leaves the working copy untouched unless it returns
         * WL_OK, so populating defaults here is safe. */
        wl_init();
        CFG->brightness = 128;
        CFG->mode = 1;
    }

    for (;;)
    {
        if (button_pressed())
        {
            CFG->brightness += 10;
            wl_mark_dirty();       /* also safe from an ISR */
        }

        wl_commit_if_dirty();      /* writes at most one record per pass */
    }
}
```

Split by design: `wl_mark_dirty()` only sets a flag and is safe to call from an
interrupt handler, while `wl_commit_if_dirty()` does the Flash work and belongs
in the main loop where blocking for a page erase is acceptable.

## API

```c
void        wl_init(void);              /* reset working copy to defaults */
wl_status_t wl_load(void);              /* restore newest usable record */
wl_status_t wl_commit(void);            /* append working copy to Flash */

void        wl_mark_dirty(void);        /* ISR-safe: request a commit */
bool        wl_is_dirty(void);
wl_status_t wl_commit_if_dirty(void);   /* main loop: commit if requested */

void       *wl_data(void);              /* mutable payload, 4-byte aligned */
uint32_t    wl_seq(void);               /* write counter of the working copy */
```

| Status | Meaning |
|---|---|
| `WL_OK` | Success. |
| `WL_EMPTY` | Region is blank — nothing has ever been written. |
| `WL_CORRUPT` | Records present, none passed its CRC. |
| `WL_BAD_VERSION` | Newest valid record has a different `WL_FORMAT_VERSION`. |
| `WL_HW_ERROR` | Controller error, timeout, or verify mismatch. Previous record intact. |
| `WL_NO_SPACE` | No writable slot, or the sequence counter is exhausted. |
| `WL_RETRY_EXHAUSTED` | Too many consecutive failures; committing is latched off. |
| `WL_BUSY` | Re-entrant call rejected. |

`WL_EMPTY` and `WL_CORRUPT` both mean "use defaults", but only the second one
says the Flash is misbehaving — worth logging separately if you have anywhere
to log to.

Neither `wl_load()` nor `wl_commit()` is re-entrant. Calling either from an
interrupt while the other is running returns `WL_BUSY` rather than corrupting
the cursor. Use `wl_mark_dirty()` from interrupt context.

## Configuration

Every setting is `#ifndef`-wrapped in `src/wl_flash_config.h` and overridable
with `-D`.

| Macro | Default | Notes |
|---|---|---|
| `WL_RECORD_SIZE` | 16 | Record size in bytes. Multiple of 4, 8..32. |
| `WL_CRC_BITS` | 32 (16 if record < 12) | CRC width. 16 buys 2 payload bytes. |
| `WL_PAGE_COUNT` | 4 | 64-byte pages in the region. At least 2. |
| `WL_FLASH_TOTAL_SIZE` | 16384 | **Set this on a 32 KB part.** |
| `WL_FLASH_BASE_ADDR` | `0x08000000` | Start of Flash. |
| `WL_FLASH_PAGE_SIZE` | 64 | Erase granularity. Do not change on CH32V00x. |
| `WL_REGION_END` | end of Flash | Only to put the region somewhere unusual. |
| `WL_FORMAT_VERSION` | 1 | Bump when your payload layout changes. |
| `WL_MAX_COMMIT_RETRY` | 3 | Consecutive failures before giving up. |
| `WL_FLASH_TIMEOUT` | 2000000 | Controller wait, in loop iterations. |
| `WL_FLASH_DEBUG` | off | `printf()` tracing. Costs ~5.3 KB — debug only. |

Do not use `DEBUG` for tracing: the vendor `debug.h` defines it to a nonzero
value whether you asked or not, which would silently turn tracing on.

### Geometry

Payload is `WL_RECORD_SIZE - 5 - WL_CRC_SIZE`. Records are laid out
`seq(4) | data | version(1) | crc`, with the payload at offset 4 so it is
4-byte aligned — RV32EC cannot do misaligned loads, so `WL_DATA_AS()` must be
able to hand out a pointer to a struct containing 32-bit fields.

| `WL_RECORD_SIZE` | Payload, CRC-32 | Payload, CRC-16 | Slots/page | Unused per page |
|---:|---:|---:|---:|---:|
| 8 | — | 1 | 8 | 0 |
| 12 | 3 | 5 | 5 | 4 |
| 16 | 7 | 9 | 4 | 0 |
| 20 | 11 | 13 | 3 | 4 |
| 24 | 15 | 17 | 2 | 16 |
| 28 | 19 | 21 | 2 | 8 |
| 32 | 23 | 25 | 2 | 0 |

Where the record size does not divide 64 evenly, the leftover bytes at the end
of each page are never addressed. Total slots is
`WL_PAGE_COUNT * WL_SLOTS_PER_PAGE`, and each page erase is amortised over
`WL_SLOTS_PER_PAGE` writes — so at a Flash endurance of roughly 10k cycles per
page, an 8-byte record buys about 80k writes per page and a 24-byte record
about 20k.

Watch out for struct padding when sizing a payload. `{uint16_t; uint8_t;
uint8_t; uint8_t;}` is 6 bytes, not 5, because the `uint16_t` forces even
alignment — `WL_ASSERT_PAYLOAD_FITS` will tell you at compile time.

### CRC

Both variants are the catalogued algorithms, so a host-side tool can validate a
Flash dump with a stock routine:

| Width | Algorithm | `check("123456789")` |
|---|---|---|
| 32 | CRC-32 (poly `0xEDB88320` reflected, init `0xFFFFFFFF`, final xor `0xFFFFFFFF`) | `0xCBF43926` |
| 16 | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no final xor) | `0x29B1` |

CRC-32 is byte-for-byte `zlib.crc32`. Both seeds are nonzero on purpose: with a
zero seed the CRC of an all-zero record is zero, so a wiped page would validate
itself and pose as a real record.

## How it works

**Writing.** The cursor advances one slot per record. On reaching the first
slot of a page, the ring has come all the way around and that page is erased
with `FLASH_CTLR_PAGE_ER` (64 bytes) — not `FLASH_CTLR_PER`, which erases a
full 1 KB sector and would wipe every page in the region plus adjacent program
Flash. The record is programmed half-word by half-word, then read back and
compared.

**Reading.** All slots are scanned; the highest sequence number with a valid
CRC wins. The 4-byte sequence number is read first and a CRC is computed only
when that number can still win, so blank and stale slots cost a compare instead
of a full checksum pass. Sequence number 0 never wins — it is reserved for
"nothing stored". Ties resolve to the first record in scan order, so the result
does not depend on scan direction.

**Recovery.** A non-blank slot in mid-page is the fingerprint of an interrupted
write. That page is *not* erased — it still holds the newest confirmed record,
and erasing it would open a window where a reset loses it. The cursor steps over
the damaged slot and keeps using the rest of the page, costing one slot rather
than a page plus an extra erase cycle.

**Sequence numbers** are 32-bit and monotonic. Wrap-around is refused rather
than handled: Flash endurance runs out many orders of magnitude before 2^32
writes.

**Interrupts** are masked for one half-word program at a time, and for the
duration of a page erase. The core stalls during a Flash operation regardless,
so masking longer than one hardware operation would add latency without buying
safety. Erase time comes from the datasheet, not from measurement here — if you
have hard real-time deadlines, measure it.

**Address validation.** The region bounds are asserted at compile time, and
both destructive HAL operations range-check at run time. The compile-time checks
catch a bad `WL_REGION_END` or `WL_FLASH_TOTAL_SIZE`; the run-time check is
there for a corrupted cursor. With 2 KB of RAM and a 256-byte stack, a single
bad word in RAM would otherwise be a primitive for erasing an arbitrary Flash
page — including live program code.

## Footprint

Measured with `riscv-wch-elf-gcc 12.2.0`, `-Os`, LTO, `-Wl,-gc-sections`,
linking the library against `scripts/size_probe.c`. Library symbols only; the
118 bytes of `memcpy`/`memcmp`/`__riscv_save` are listed separately because a
real application almost certainly pays for them anyway.

| Record | CRC | Payload | Flash | RAM |
|---:|---:|---:|---:|---:|
| 8 | 16 | 1 | 900 B | 28 B |
| 12 | 32 | 3 | 896 B | 32 B |
| 12 | 16 | 5 | 906 B | 32 B |
| 16 | 32 | 7 | 914 B | 36 B |
| 16 | 16 | 9 | 924 B | 36 B |
| 20 | 32 | 11 | 906 B | 40 B |
| 24 | 32 | 15 | 910 B | 44 B |
| 28 | 32 | 19 | 906 B | 48 B |
| 32 | 32 | 23 | 904 B | 52 B |

Plus 118 B of libc/ABI helpers and, with `-DWL_FLASH_DEBUG`, about 5.3 KB of
`printf` and format strings.

No configuration links a division helper from libgcc. RV32EC has neither a
divider nor a multiplier, so all slot addressing is incremental; CI asserts
that no `__udivsi3`/`__umodsi3` sneaks back in.

Bundled examples build to 8.5–8.9 KB with tracing enabled, which is dominated
by `printf`.

For reference, v1.6.1 was 776 B (or 662 B with `WL_FLASH_LIGHT`). The default
16-byte configuration is 138 B larger. That buys, per ablation measurement:
run-time address validation (44 B), verify-after-write (40 B), the bounded
retry and re-entrancy guards (28 B), and the blank-slot check during
programming (14 B). The seq-gated scan is 6 B *smaller* than checksumming every
slot, and faster.

## Testing

```bash
pio test -e native      # host suite: no hardware needed
pio run                 # build all three examples for the target
pio run -e size_r16     # library-only footprint
```

The suite runs the real `wl_flash.c` against a RAM model of Flash that enforces
the rules that matter — erase sets `0xFF`, programming only clears bits,
re-programming a written half-word fails — and injects the failures you cannot
provoke on demand on real hardware:

- power loss at **every** half-word boundary of a record write, including at a
  page boundary, verifying afterwards that the previous record still loads and
  that the store still accepts new records
- a page erase that reports failure
- a controller that reports success but stores wrong data

25 cases, roughly 400 assertions, all geometry-derived so the same code covers
every record size. CI runs them across 12 combinations of record size, CRC
width and page count, plus a target build for all 7 record sizes.

The suite was validated by mutation testing: each of the bugs fixed in 2.0.0
was deliberately reintroduced, and each is caught by a named test. Two of them
— reusing a page on a mid-page anomaly, and flat page addressing that ignores
the unused tail bytes — are real regressions from the project's history.

Adding a test case means adding a function and one `WL_RUN()` line in
`test/test_wl/test_main.c`. No framework beyond `wl_test.h`.

## Porting

All Flash access goes through four primitives in `src/wl_flash_hal.h`:

```c
bool wl_hal_is_erased(uint32_t addr, uint32_t len);
void wl_hal_read(uint32_t addr, void *dst, uint32_t len);
bool wl_hal_program(uint32_t addr, const void *src, uint32_t len);
bool wl_hal_verify(uint32_t addr, const void *src, uint32_t len);
bool wl_hal_erase_page(uint32_t addr);
```

Define `WL_FLASH_HAL_MOCK` to compile out the CH32V back end and supply your
own. `src/wl_flash_hal_ch32v.c` is about 200 lines and is the only file that
knows anything about the hardware.

## Migrating from 1.x

The API is entirely new; nothing from 1.x compiles. The on-flash format also
changed — CRC-32 gained its missing final XOR — so **existing stored records
will not validate**. `wl_load()` returns `WL_CORRUPT` and the application falls
back to defaults. If that matters, read the old records out before upgrading.

| 1.x | 2.0 |
|---|---|
| `read_ee()` → `1`/`0` | `wl_load()` → `wl_status_t` |
| `write_ee()` → `1`/`0` | `wl_commit()` → `wl_status_t` |
| `init_ee()` | `wl_init()` |
| `handle_ee()` | `wl_commit_if_dirty()` |
| `need_save = 1` (app-defined global) | `wl_mark_dirty()` |
| `ee.user[]`, union overlay | `wl_data()`, `WL_DATA_AS(type)` |
| `ee.seq` | `wl_seq()` |
| `EE_STRUCT_SIZE` | `WL_RECORD_SIZE` |
| `EEPROM_PAGE_COUNT` | `WL_PAGE_COUNT` |
| `PAGE_WRITE_END_ADDR` | `WL_REGION_END` |
| `EE_USER_SIZE` | `WL_DATA_SIZE` |
| `config.h` | `wl_flash_config.h` |
| `WL_FLASH_LIGHT` | removed |

The union-overlay pattern the old examples used is gone. `WL_DATA_AS()` returns
a pointer to the payload, so a plain struct is enough — no need to restate the
header fields and keep them in sync.

`WL_FLASH_LIGHT` was removed rather than carried forward. It saved 152 bytes by
dropping verify-after-write and the blank-slot checks — the checks that catch
silent Flash corruption in a library whose only job is not losing data.

See [CHANGELOG.md](CHANGELOG.md) for the full list, including the bugs this
release fixes.

## Requirements

WCH CH32V003 (RV32EC), PlatformIO, `noneos-sdk` framework. Host tests need any
C11 compiler.

## License

MIT — see [LICENSE](LICENSE).
