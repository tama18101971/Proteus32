# AUX command 32-bit word format (include/aux_leds.h)

## Rule (adopted from Proteus3D, 2026-08-31)
The AUX command word is serialised into Flash arrays (strobe_commands, beacon_double_buf),
so it is NEVER decoded through C bitfields — bitfield layout is implementation-defined
(ABI/compiler-dependent). All access goes through explicit mask/shift macros.
`union AUX_Cmd_u` holds only `uint32_t aux_cmd`.

## Bit layout (LED command, bits 5..31 != 0)
- [2:0]   device        AUX_DEVICE_*    1=RED, 2=GRN, 4=HBL (combinable)
- [3]     cycle         AUX_CYCLE_*     0=once, 1=looped
- [7:4]   count         AUX_COUNT_*     repeats 0..15
- [10:8]  psc0          AUX_PSC0_*      AWU prescaler index, 1st half-period
- [16:11] win0          AUX_WIN0_*      AWU window 0..63
- [19:17] psc1          AUX_PSC1_*
- [25:20] win1          AUX_WIN1_*
- [28:26] brightness0   AUX_BR0_*       modes[] index / AUX_HBL_DIRECT_STEP
- [31:29] brightness1   AUX_BR1_*

## System command (bits 5..31 == 0) — different from Proteus3D (which uses bit31 marker)
- [1:0] opcode AC_STOP/AC_REP_ATSTART/AC_REP_ATSTEPS (AUX_SYS_CMD_OP)
- [4:2] steps for AC_REP_ATSTEPS (AUX_SYS_CMD_STEPS)
- Build: MAKE_SYS_CMD(op, steps); test: AUX_IS_SYS_CMD(word) == ((word & 0xFFFFFFE0) == 0)
- Plain array elements like `AC_REP_ATSTART` are valid system commands as-is.

## Compile-time guards in aux_leds.h (do not remove)
- sizeof(union AUX_Cmd_u) == 4; sizeof(AUX_Ctr_t) == 4
- Pairwise mask non-overlap asserts + masks tile 0xFFFFFFFF exactly
- Golden round-trip: MAKE_CMD(0x7,1,15,7,63,7,63,7,7) == 0xFFFFFFFF and decodes back
- Regression freeze: INFO_RED == 0x0789C311, INFO_GRN == 0x0783C222,
  ERRO_RED == 0x07C07831, DANG_RED == 0x07C07811
  (asserts fold to constants — zero Flash cost; verified 2026-08-31:
  Flash 15928/16128 vs 15936 baseline, LSP clangd include-path errors in IDE are pre-existing noise)

## AUX_Ctr_t
Internal (never serialised) state — bitfields allowed there; only size is pinned.
