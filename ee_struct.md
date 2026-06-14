# EEPROM over Flash — Data Structure and Algorithm

## Contents

1. [Overview](#1-overview)
2. [Structure `ee_data`](#2-structure-ee_data)
3. [Structure Requirements](#3-structure-requirements)
4. [Flash Layout](#4-flash-layout)
5. [Algorithm](#5-algorithm)
6. [Version Field](#6-version-field)
7. [Power-Loss Safety](#7-power-loss-safety)
8. [Expanding Storage](#8-expanding-storage)
9. [Modification Checklist](#9-modification-checklist)

---

## 1. Overview

CH32V003 has no hardware EEPROM. Data is emulated using the last 256 bytes of flash (16 KB chip) with a **linear EEPROM algorithm** — data is written sequentially into slots; when all slots are full, all flash pages are erased and the cycle repeats.

**Key specs:**
- Flash chip: 16 KB (0x08000000 .. 0x08003FFF)
- EEPROM region: 256 bytes (0x08003F00 .. 0x08003FFF) = 4 pages × 64 bytes
- Struct size: 32 bytes (exactly 8 flash words)
- Slots: 8 (256 / 32 = 8)
- Data integrity: each record contains CRC32 + sequence number + version
- Erase: 1KB page erase via PER register bit (not 64-byte PAGE_ER)
- Flash write: direct register access (PG bit, two 16-bit halfwords per word)
- CRC: nibble-based lookup table (16 entries, compact and fast)
- All peripheral access: bare-metal registers (no SDK functions)

---

## 2. Structure `ee_data`

### 2.1. Layout (32 bytes)

```
Offset  Size  Field             Description
------  ----  -----             -----------
  0       4   seq               Sequence number (determines latest record)
  4       1   version           Struct version (firmware compatibility)
                                WARNING: packed uint8_t bug — use raw byte access
  5       2   defauil_ui1       Default UI value (part 1)
  7       1   uif               UI flags (packed bitfield)
  8       1   aux               Backlight/beacon flags (packed bitfield)
  9       3   eng               Engineering flags (packed bitfield)
 12       1   usr               User flags (packed bitfield)
 13       1   defauil_ui2       Default UI value (part 2)
 14       1   defauil_ui3       Default UI value (part 3)
 15      10   slots[10]         Data slot array
 25       3   reserved          Padding to maintain 32-byte size
 28       4   crc               CRC32 of preceding 28 bytes (seq..reserved)
------  ----
Total:  32 bytes
```

### 2.2. Bitfield Details

```c
// aux_flags (1 byte, offset 8)
bcklight_type  : 1   // Backlight type (0 or 1)
bcklight_color : 1   // Backlight color (0 or 1)
beacon_on      : 1   // Beacon enabled (0=off, 1=on)
beacon_mode    : 2   // Beacon mode (0..3)
beacon_type    : 3   // Beacon type (0..7)

// eng_flags (3 bytes, offset 9)
min_level : 5         // Minimum level (0..31)
vc        : 7         // Voltage correction (-64..+63)
tc        : 7         // Temperature correction (-64..+63)
reserved  : 5         // Reserved

// usr_flags (1 byte, offset 12)
t_calibr   : 4        // Temperature calibration (0..15)
eaux_onoff : 1        // Extra screen on/off
id_plus_sid: 1        // ID + SID mode
reserved   : 2        // Reserved

// ui_flags (1 byte, offset 7)
ui_index   : 2        // Interface index (0..3)
off_bckl   : 1        // Backlight off
use_memory : 1        // Use memory
reserved   : 4        // Reserved
```

---

## 3. Structure Requirements

### 3.1. Size Must Be Multiple of 4 (CRITICAL)

```c
sizeof(struct ee_data) MUST be a multiple of 4 (DATAWIDTH)
```

**Why:**
- Read/write operates via direct register access — 4 bytes (2 × 16-bit halfwords) at a time
- Write loop: `for (i = 0; i < sizeof / 4; i++)` — writes `sizeof/4` words
- If sizeof is not a multiple of 4, the last byte of CRC won't be written/read
- On next read, `temp.crc` gets garbage in the MSB → CRC check always fails

**How to ensure:**
- Sum all fields to a multiple of 4
- If short, add `uint8_t reserved[N]` padding
- Verify: `4+1+2+1+1+3+1+1+1+10+3+4 = 32` ✓

### 3.2. Compiler Attributes

```c
struct __attribute__((packed, aligned(4))) ee_data { ... };
```

- `packed` — remove padding between fields (otherwise compiler inserts alignment bytes)
- `aligned(4)` — align struct to 4 bytes in memory (needed for flash word operations)

### 3.3. Field Order

Fields **must** appear in the order they are written to flash. CRC is computed over fields `seq` .. `reserved` (28 bytes) and stored in `crc` (last 4 bytes).

Field order determines:
- Which bytes are included in CRC
- How data maps to flash words
- Firmware upgrade compatibility

---

## 4. Flash Layout

### 4.1. Memory Map

```
0x08000000 ┌─────────────────────────────┐
           │                             │
           │     Program (16000 bytes)   │
           │                             │
0x08003EFF ├─────────────────────────────┤
0x08003F00 │  Slot 0 (32 bytes)         │  ← Page 1 (64 bytes)
0x08003F20 │  Slot 1 (32 bytes)         │
0x08003F40 │  Slot 2 (32 bytes)         │  ← Page 2 (64 bytes)
0x08003F60 │  Slot 3 (32 bytes)         │
0x08003F80 │  Slot 4 (32 bytes)         │  ← Page 3 (64 bytes)
0x08003FA0 │  Slot 5 (32 bytes)         │
0x08003FC0 │  Slot 6 (32 bytes)         │  ← Page 4 (64 bytes)
0x08003FE0 │  Slot 7 (32 bytes)         │
0x08003FFF └─────────────────────────────┘
```

### 4.2. Flash Pages

| Page | Start      | End        | Slots |
|------|------------|------------|-------|
| 1    | 0x08003F00 | 0x08003F3F | 0, 1  |
| 2    | 0x08003F40 | 0x08003F7F | 2, 3  |
| 3    | 0x08003F80 | 0x08003FBF | 4, 5  |
| 4    | 0x08003FC0 | 0x08003FFF | 6, 7  |

- CH32V003 page size: **64 bytes**
- EEPROM region: **256 bytes** (4 pages)
- Slots: **8** (256 / 32 = 8)

### 4.3. Erase Policy

Flash is erased in **1KB pages** via the PER (Page Erase) register bit. Two scenarios:

**A. Page-by-page erase (normal operation):**
When writing to the FIRST slot of a page (address is page-aligned):
1. Check if page is already erased (`is_page_erased`)
2. If not erased — erase only that 1KB page
3. Other pages remain untouched (data preserved)

**B. Full erase (all slots occupied):**
When all 8 slots are full:
1. All 4 pages are erased sequentially (each triggers a 1KB erase)
2. Writing restarts from slot 0

**Why page-by-page:**
- Reduces flash wear (only 1KB erased vs multiple pages)
- Better power-loss safety (only 1 page affected, not all 4)
- Other pages serve as backup until overwritten

---

## 5. Algorithm

### 5.1. Initialization (first boot)

```
flash empty → read_ee() returns 0
             → init_ee() fills ee with defaults
             → version = EE_VERSION
             → seq = 0
```

### 5.2. Write (every RESET)

```
┌─────────────────────────────────────────────────────┐
│ 1. read_ee() — find last valid record                │
│    flash_search_address() scans slots 0..7            │
│    → ResAddress = first free slot address             │
│    → MaxSeq = highest seq among records               │
├─────────────────────────────────────────────────────┤
│ 2. If flash empty → init_ee() (defaults)              │
├─────────────────────────────────────────────────────┤
│ 3. write_ee() — save:                                │
│    a. ee.seq = MaxSeq + 1                            │
│    b. ee.crc = CRC32(seq..reserved)                  │
│    c. Use ResAddress from read_ee (no re-scan)       │
│    d. If ResAddress == 0 → scan again or erase all   │
│    e. If first slot of page → erase page if needed   │
│    f. __disable_irq()                                │
│    g. Unlock flash, write 8 words via registers      │
│    h. flash_wait_bsy() + Lock flash                  │
│    i. __enable_irq()                                 │
└─────────────────────────────────────────────────────┘
```

### 5.3. Read (read_ee)

```
flash_search_address() → finds last valid slot
    ↓
Copy from flash to RAM
    ↓
CRC32(entire struct including crc) == 0?
    ├── No  → data corrupted, return error
    └── Yes ↓
version == EE_VERSION?
    ├── No  → firmware mismatch, return error
    └── Yes → copy to ee, return success
```

### 5.4. Slot Search (flash_search_address)

```
current = PAGE_WRITE_START_ADDR (0x08003F00)

┌─► current <= PAGE_WRITE_END_ADDR - 32 ?
│       ├── Yes ─► is_slot_valid(current)?
│       │           ├── Yes ─► read seq, update MaxSeq
│       │           │          current += 32 → next slot
│       │           └── No  → write_addr = current (free)
│       │                     break → return write_addr
│       └── No  → all slots occupied, return 0
└─────────────────────────────────────────────────┘
```

### 5.5. Slot Validity Check (is_slot_valid)

```
First byte == 0xFF?  ──► Yes → slot empty (erased flash)
         │
         No
         ↓
Copy 32 bytes from flash
         ↓
CRC32(first 28 bytes) == temp.crc?
         ├── Yes → slot valid ✓
         └── No  → data corrupted ✗
```

### 5.6. Erase (erase_flash_page)

```
__disable_irq()
Unlock flash (FLASH->KEYR = KEY1, KEY2)
flash_wait_bsy()
Clear error flags
Erase 1KB page (FLASH->CTLR |= PER, ADDR = page, CTLR |= STRT)
flash_wait_bsy()
Lock flash (FLASH->CTLR |= LOCK)
__enable_irq()
```

---

## 6. Version Field

### 6.1. Purpose

The `version` field (1 byte at offset 4) protects against firmware incompatibility. When the struct layout changes between firmware versions, the version is incremented. Old records with a different version are automatically rejected.

### 6.2. Compiler Bug Workaround

**CH32V003 GCC has a bug:** reading a `uint8_t` field from a `packed` struct returns 0 instead of the actual value. This affects the `version` field.

**Solution:** Access `version` via raw byte pointer instead of struct field:

```c
// Writing version:
uint8_t *ee_raw = (uint8_t*)(void*)&ee;
ee_raw[sizeof(uint32_t)] = EE_VERSION;  // offset 4

// Reading version:
uint8_t *raw_bytes = (uint8_t*)(void*)&temp_ee;
uint8_t file_version = raw_bytes[sizeof(uint32_t)];  // offset 4
if (file_version != EE_VERSION) { ... }
```

**Why offset 4:** `version` is placed after `seq` (uint32_t, 4 bytes) to ensure word-alignment for the first flash word.

### 6.3. When to Increment

Increment `EE_VERSION` when:

| Change | Increment? | Reason |
|--------|-----------|--------|
| Add/remove field | Yes | Layout changed, old data misinterpreted |
| Reorder fields | Yes | CRC computed over different bytes |
| Change field size | Yes | Layout changed |
| Change default values | No | Layout unchanged, just defaults |
| Rename field | No | Layout unchanged |
| Change bitfield layout | Yes | Byte-level representation changed |

### 6.4. Migration Strategy

When incrementing version, old data is **automatically rejected** (treated as empty → `init_ee` loads defaults). This is safe because:

1. CRC check passes (data is intact)
2. Version check fails → `read_ee` returns 0
3. `init_ee` loads fresh defaults
4. User settings are lost, but device works

**For critical data that must survive upgrades:**

```c
// Option 1: Add migration code
int read_ee(void) {
    // ... CRC check ...
    if (temp_ee.version == 1) {
        // Migrate v1 → v2
        migrate_v1_to_v2(&temp_ee);
    } else if (temp_ee.version == EE_VERSION) {
        // Current version — no migration needed
    } else {
        return 0;  // Unknown version
    }
    memcpy(&ee, &temp_ee, sizeof(ee));
    return 1;
}

// Option 2: Keep backward compatibility
// Don't change the struct layout — only add new fields at the end
// Old firmware ignores new fields (they're in reserved space)
```

---

## 7. Power-Loss Safety

### 7.1. Power loss during page erase

**Scenario:** Page erase complete, new write not started.

**Result:**
- One page erased (0xFF)
- Other 3 pages intact (data preserved)
- On next boot: `flash_search_address` continues from intact data
- Erased page's slots are considered empty → skipped

**Verdict:** Safe — only the erased page's data is lost, other pages preserved.

### 7.2. Power loss during full erase (all slots full)

**Scenario:** 2 of 4 pages erased, power lost.

**Result:**
- Some pages erased, others intact
- On next boot: valid slots on intact pages are used
- Erased pages are skipped

**Verdict:** Partial data loss — but previous records on intact pages survive.

### 7.3. Power loss during write (word-by-word)

**Scenario:** 3 of 8 words written, power lost.

**Result:**
- Slot contains partial data + 0xFF (unprogrammed)
- CRC check fails → slot considered invalid
- On next boot: slot skipped, previous valid record used

**Verdict:** Safe — old data preserved.

### 7.4. Power loss between read_ee and write_ee

**Scenario:** data read, power lost before write.

**Result:**
- Flash unchanged (no write happened)
- On next boot: reads same data as before

**Verdict:** Safe — no data loss.

### 7.5. Summary

| Scenario | Data Loss? | Recovery |
|----------|-----------|----------|
| During single page erase | 1 page only | Other pages intact |
| During full erase | Up to 2 pages | Other pages intact |
| During write | Current record | Previous record intact |
| Between read/write | None | Same as before |

---

## 8. Expanding Storage

### 8.1. Current Configuration (256 bytes)

Already configured with 256 bytes EEPROM (4 pages, 8 slots).

### 8.2. Reducing to 128 bytes (2 pages, 4 slots)

If program space is critical:

```c
// Change start address back to 128-byte region
#define PAGE_WRITE_START_ADDR  ((uint32_t)0x08003F80)  // Was 0x08003F00
```

In `platformio.ini`:
```ini
build_flags =
    -Wl,--defsym=FLASH_SIZE=16256    ; 16384 - 128 = 16256
```

### 8.3. Larger Struct

Add new fields while keeping multiple of 4:

```c
struct __attribute__((packed, aligned(4))) ee_data {
    uint32_t seq;               // 4 bytes (word-aligned)
    uint8_t version;            // 1 byte (use raw byte access)
    // ... existing fields ...
    uint8_t new_field1;          // +1 byte
    uint8_t new_field2;          // +1 byte
    // Adjust reserved to maintain multiple of 4
    uint32_t crc;                // 4 bytes
};
```

**Consequences:**
- Slots on 256-byte region: 256 / sizeof(struct) = fewer slots
- CRC computed over (sizeof - 4) bytes
- Reduced endurance (fewer slots → more frequent erase)
- **Remember to increment EE_VERSION**

---

## 9. Modification Checklist

### Before changing the structure

- [ ] `sizeof(struct ee_data)` is multiple of 4?
- [ ] CRC computed over all fields BEFORE `crc`?
- [ ] Fields in write order (memory order = code order)?
- [ ] `packed` and `aligned(4)` present?
- [ ] Slot count recalculated?
- [ ] `EE_VERSION` incremented?
- [ ] Old data compatible? (if not → add migration code)
- [ ] **New `uint8_t` fields?** Use raw byte access (packed uint8_t bug)

### Common "Storage is EMPTY" bugs

1. **sizeof not multiple of 4** — CRC not fully written/read
2. **No `packed`** — compiler inserts padding, real size larger than expected
3. **Fields reordered** — CRC computed over different bytes
4. **`reserved` in wrong place** — breaks alignment and size
5. **Packed `uint8_t` field** — reads as 0 on CH32V003 GCC (use raw bytes)

### How to add a new field

1. Determine field size (1, 2, or 4 bytes)
2. Add after last field, before `reserved` or `crc`
3. Adjust `reserved` to maintain 32-byte size (or grow struct)
4. Verify `sizeof` is multiple of 4
5. Increment `EE_VERSION`
6. Update `init_ee()` — set default value
7. Update `wl_flash.h` — add comment with offset/size
8. If old data must survive — add migration code in `read_ee()`
9. **If field is `uint8_t`** — access via `((uint8_t*)&struct)[offset]`
