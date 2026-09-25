"""Post-build flash/RAM size gate for Proteus32.

Prints the firmware.bin size vs. the 16128-byte flash limit, warns when the
remaining margin drops below WARN_MARGIN, fails the build when ERROR_AT is
exceeded, and lists the largest flash-resident input sections (.text.*,
.rodata.*) from the linker map to make regressions visible.
A static-RAM line is printed as well, because -fmerge-all-constants and similar
flags move constants from flash into RAM.

The Top-N scan only walks the .text output section, so RAM-resident constants
(.srodata* lands in .data per Link.ld) are covered by the RAM total rather than
by the symbol list.

ERROR_AT is the hard gate: exceeding it fails the build. It is set to the
flash limit (HARD_LIMIT, 16128 B — matches board_upload.maximum_size and
-Wl,--defsym=FLASH_SIZE): the image must fit the physical flash.
"""

import os
import re

Import("env")

WARN_MARGIN = 256
HARD_LIMIT = 16128
ERROR_AT = HARD_LIMIT  # fail the build only when the flash limit is exceeded
TOP_N = 15

# RAM accounting (see Link.ld: statics start at ORIGIN(RAM) and run up to
# _ebss; the stack is reserved at the top of RAM, so the statics budget is
# RAM_SIZE - STACK_RESERVE).
RAM_SIZE = 2048
RAM_ORIGIN = 0x20000000
STACK_RESERVE = 512

_TEXT_OUT = re.compile(r"^\.text\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s*$")
# Only flash-resident input sections: the walk is gated on the .text output
# section, and Link.ld puts .srodata* in .data (RAM), where it never matches.
_TEXT_NAME = re.compile(r"^\s+\.(?:text|rodata)\.([^\s]+)\s*$")
_TEXT_NAME_SIZE = re.compile(
    r"^\s+\.(?:text|rodata)\.([^\s]+)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+\S"
)
_TEXT_CONT = re.compile(r"^\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)\s+\S")

_EBSS = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+(?:\[!provide\]\s+)?PROVIDE \(_ebss = \.\)\s*$")
_STACK = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+__stack_size = 0x([0-9a-fA-F]+)\s*$")


def _print(line):
    try:
        print(line)
    except UnicodeEncodeError:
        print(line.encode("ascii", "replace").decode("ascii"))


def _top_text_symbols(map_path, top_n):
    symbols = {}
    in_text = False
    pending = None
    with open(map_path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            if line[0] not in " \t":
                in_text = bool(_TEXT_OUT.match(line))
                pending = None
                continue
            if not in_text:
                continue
            match = _TEXT_NAME_SIZE.match(line)
            if match:
                name, size = match.group(1), int(match.group(2), 16)
                symbols[name] = symbols.get(name, 0) + size
                pending = None
                continue
            match = _TEXT_NAME.match(line)
            if match:
                pending = match.group(1)
                continue
            if pending:
                match = _TEXT_CONT.match(line)
                if match:
                    size = int(match.group(1), 16)
                    symbols[pending] = symbols.get(pending, 0) + size
                pending = None
    return sorted(symbols.items(), key=lambda item: item[1], reverse=True)[:top_n]


def _ram_usage(map_path):
    """Return (statics_bytes, stack_bytes) or None when the map lacks markers."""
    ebss = None
    stack = STACK_RESERVE
    with open(map_path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if ebss is None:
                match = _EBSS.match(line)
                if match:
                    ebss = int(match.group(1), 16) - RAM_ORIGIN
                    continue
            match = _STACK.match(line)
            if match:
                stack = int(match.group(2), 16)
    if ebss is None:
        return None
    return ebss, stack


def _after_build(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("$PROGNAME")
    bin_path = target[0].get_abspath() if target else os.path.join(
        build_dir, progname + ".bin"
    )
    limit = int(env.BoardConfig().get("upload.maximum_size", HARD_LIMIT))
    size = os.path.getsize(bin_path)
    free = limit - size

    _print("Size: {} / {} (free {})".format(size, limit, free))

    map_path = os.path.join(build_dir, progname + ".map")
    if os.path.isfile(map_path):
        _print("Top-{} .text/.rodata symbols:".format(TOP_N))
        for name, symbol_size in _top_text_symbols(map_path, TOP_N):
            _print("  {:>6}  {}".format(symbol_size, name))

        usage = _ram_usage(map_path)
        if usage is not None:
            statics, stack = usage
            budget = RAM_SIZE - stack
            _print(
                "RAM: {} / {} (free {})  [{} + {} stack = {} / {}]".format(
                    statics,
                    budget,
                    budget - statics,
                    statics,
                    stack,
                    statics + stack,
                    RAM_SIZE,
                )
            )

    if free < 0 or size > ERROR_AT:
        _print(
            "ERROR: firmware.bin {} B exceeds gate threshold {} B".format(
                size, ERROR_AT
            )
        )
        env.Exit(1)
    elif free < WARN_MARGIN:
        _print(
            "WARNING: flash margin {} B < {} B (gate {} B)".format(
                free, WARN_MARGIN, ERROR_AT
            )
        )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _after_build)

# The ch32v upload target flashes ${PROGNAME}.elf and does not depend on the
# .bin, which would skip this gate for `pio run -t upload`.  Make the image
# (and therefore the check above) a dependency of upload.
env.Depends("upload", "$BUILD_DIR/${PROGNAME}.bin")
