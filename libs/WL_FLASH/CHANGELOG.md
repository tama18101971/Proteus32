# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/).

## [2.0.1]

Patch release. No library source changes, no API changes, no on-flash format
changes — anything built against 2.0.0 keeps working.

### Fixed

- **The region guard could not find `wl_flash_config.h` when run as a
  `lib_deps` extraScript,** so it silently used the hardcoded fallback copies
  of the defaults instead of reading the header. Harmless while the two agreed,
  wrong as soon as someone edited a default in the header. It now uses the
  `pio_lib_builder.path` that PlatformIO exports to extraScripts.
- **CI did not install the `ch32v` platform.** It is a community platform and
  is not in the PlatformIO registry, so every target job on a clean runner
  failed with `UnknownPackageError: Could not find the package with 'ch32v'
  requirements`. Invisible locally, where the platform is already installed.

## [2.0.0]

Full rewrite following an audit. The API is entirely new — v1.x code will not
compile against this release. See "Migrating from 1.x" in the README.

### Fixed

- **A commit without a preceding load destroyed the staged record.** The write
  path located its cursor with a helper that also loaded the record it found,
  overwriting the caller's working copy. `wl_init(); …stage data…; commit;`
  silently wrote the old Flash contents back with a fresh sequence number, so a
  factory reset never took effect and the staged data was lost. Cursor lookup
  and record loading are now separate; covered by
  `test_commit_without_load_keeps_staged_data`.
- **A factory reset could lose to the record it replaced.** `init_ee()` zeroed
  the sequence counter along with the payload, so the next write could reuse a
  number already on Flash and lose the tie. `wl_init()` now leaves the stored
  sequence number alone; covered by
  `test_factory_reset_outranks_stored_record`.
- **The dirty flag could not be set safely from an interrupt.** `need_save` was
  declared `extern uint8_t` in the library, so declaring it `volatile` in the
  application was a hard compile error ("conflicting type qualifiers") — while
  leaving it non-volatile allowed the compiler to cache it in a register and
  never observe the ISR's write. The flag is now library-owned and volatile,
  reached through `wl_mark_dirty()` / `wl_commit_if_dirty()`.
- **An all-zero record validated itself.** The 8-byte layout used
  CRC-16/XMODEM, seeded at 0x0000, for which the CRC of all-zero data is
  0x0000. A wiped page therefore read back as a genuine record carrying
  sequence number 0. Both CRCs now use nonzero seeds; covered by
  `test_zeroed_region_is_not_valid`.
- **Fast program/erase mode was left unlocked.** The erase path unlocked it via
  `MODEKEYR` but only ever set `FLASH_CTLR_LOCK`, never `FLOCK`, so after the
  first erase 64-byte page erase stayed available to any stray write for the
  rest of the run.
- **Interrupt save/restore was asymmetric.** `csrrci mstatus, 8` saved MIE
  only, but the restore called the vendor `__enable_irq()`, which sets mstatus
  bits 3 *and* 7 and so also raised MPIE. The inline asm also lacked a
  `"memory"` clobber, leaving the compiler free to move accesses across the
  critical section. Now exactly the saved bit is restored, with barriers.
- **Nothing validated the target address before an erase.** The old code drove
  the Flash controller directly and skipped the range check the vendor
  `FLASH_ErasePage_Fast()` performs, so a corrupted cursor was a primitive for
  erasing an arbitrary Flash page, including live program code. Both
  destructive HAL operations now range-check, and the region bounds are
  asserted at compile time.
- **The `basic` and `basic8` examples destroyed the record they had just
  read** by calling `init_ee()` unconditionally after a successful read.

### Changed

- **A mid-page anomaly costs one slot instead of a whole page.** Finding a
  half-written record inside a page used to divert the entire page and erase
  the next one, wasting up to `WL_SLOTS_PER_PAGE - 1` usable slots and one
  extra erase cycle per unclean reset — up to 4× write amplification in the
  default geometry. The cursor now steps over the damaged slot and keeps using
  the rest of the page. The power-loss guarantee is unchanged: the page holding
  the newest confirmed record is still never erased.
- **No more division or modulo at run time.** RV32EC has neither a divider nor
  a multiplier, so the old `idx / SLOTS_PER_PAGE` addressing pulled
  `__udivsi3`/`__umodsi3` out of libgcc — 128 bytes of Flash, for the 12- and
  20-byte record sizes. Addressing is now an incremental cursor; CI asserts no
  division helper is linked.
- **Start-up scans faster.** The scan reads the 4-byte sequence number first
  and only computes a CRC when that number can still win, instead of
  checksumming every non-blank slot.
- **`read`/`write` return a status instead of `int`.** `wl_status_t`
  distinguishes a blank region, a corrupt region, a version mismatch, and a
  hardware error, which the old `0`/`1` conflated.
- **A failing commit no longer retries forever.** `wl_commit_if_dirty()` gives
  up after `WL_MAX_COMMIT_RETRY` consecutive failures and reports
  `WL_RETRY_EXHAUSTED`, instead of hammering a worn-out or write-protected
  Flash with erase cycles on every pass of the main loop.
- **CRCs are now the catalogued algorithms.** CRC-32 gained its missing final
  XOR, so it matches `zlib.crc32` and Ethernet FCS
  (`check("123456789") == 0xCBF43926`); the 16-bit variant is CRC-16/CCITT-FALSE
  (`0x29B1`). A host-side tool can now validate a Flash dump with a stock CRC
  routine. **This is an on-flash format change: records written by 1.x will not
  validate.**
- **Interrupts are masked per half-word, not per record.** The old code held
  them off for an entire slot program; worst-case latency is now one half-word
  program, plus the page erase where masking is unavoidable.
- **CRC width is selectable independently of record size** via `WL_CRC_BITS`.
  Choosing 16 buys 2 extra payload bytes at any record size.
- **`WL_FORMAT_VERSION` is overridable**, so an application can bump the
  on-flash version when its payload layout changes. It was hard-wired to 1.
- **The example `platformio.ini` files pin `lib_deps` to a tag** instead of
  tracking the default branch.

### Added

- **Host test suite** (`pio test -e native`): 25 cases, ~400 assertions,
  running the real algorithm against a RAM model of Flash. Fault injection
  covers power loss at every half-word boundary of a record write, erase
  failure, and a controller that reports success while storing wrong data.
  Validated by mutation testing — each deliberate reintroduction of the bugs
  above is caught by a named test.
- **Build-time region guard** (`scripts/reserve_eeprom.py`, wired in through
  `library.json`). Reserving the EEPROM region from the linker used to be a
  documentation footnote: get `board_upload.maximum_size` wrong and firmware
  lands inside the region, where the first commit erases it. The script
  recomputes the value and fails the build with the correct number. It also
  catches `WL_FLASH_TOTAL_SIZE` left at the 16 KB default on a larger part,
  which would place the region in the middle of Flash.
- **HAL seam** (`wl_flash_hal.h`): four primitives isolate all Flash access,
  which is what makes the host suite possible and lets the library be ported
  by swapping one file.
- **CI** (GitHub Actions): host suite, host suite across 12 geometry
  combinations, target builds for all 7 record sizes, the libgcc-division
  assertion, and cppcheck.
- **`WL_ASSERT_PAYLOAD_FITS()`** so an oversized payload struct fails at
  compile time with a readable message.
- Compile-time assertions that the region lies inside physical Flash and is
  page-aligned at both ends.

### Removed

- **`WL_FLASH_LIGHT`.** It saved 152 bytes by dropping verify-after-write and
  the blank-slot checks — that is, by dropping the checks that catch silent
  Flash corruption in a library whose entire purpose is not losing data. It
  also doubled the test matrix for every future change.
- `struct ee_data`, `ee`, `need_save`, `read_ee()`, `write_ee()`, `init_ee()`,
  `handle_ee()`, `EE_STRUCT_SIZE`, `EEPROM_PAGE_COUNT`, `PAGE_WRITE_END_ADDR`,
  `slot_state_t`, `DATAWIDTH`, `GET_PAGE_ADDR`, `EE_VERSION`.
- `src/config.h`. A library header with that name collides with the
  application's own `config.h` — whichever the include path finds first wins.
  Configuration now lives in `wl_flash_config.h`.

## [1.6.1] and earlier

See the git history. 1.3.0 fixed page-aware slot addressing (flat addressing
drifted into the unused tail bytes of each page and caused endless anomaly
loops); 1.5.0 unified anomaly handling with page failover.
