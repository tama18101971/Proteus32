"""
reserve_eeprom.py — verify that the EEPROM region is excluded from the
firmware link image, and that it actually sits at the end of Flash.

WL_FLASH stores records in the last WL_PAGE_COUNT * WL_FLASH_PAGE_SIZE bytes
of Flash. Nothing in the toolchain knows that, so unless the link image is
capped the linker will place code there and the first commit erases it.

PlatformIO does not evaluate arithmetic in platformio.ini, so
board_upload.maximum_size has to be a literal. This script recomputes the
right value from the build flags and the board manifest, then fails the build
when it is missing or wrong — turning a silent runtime disaster into a build
error carrying the number to paste in.

Two mistakes it catches:

  1. maximum_size not reduced (or not reduced enough), leaving firmware and
     records fighting over the same bytes.
  2. WL_FLASH_TOTAL_SIZE left at its 16 KB default on a larger part, which
     puts the region in the middle of Flash rather than at the end — i.e.
     underneath live program code.

Registered from library.json (extraScript) so it also runs for consumers who
pull the library in through lib_deps.
"""

import os
import re

from SCons.Script import Import

Import("env")

# Exported only when this runs as a lib_deps extraScript, where it is the most
# reliable way to find the library root. Absent in the unified root build,
# which registers the script as a pre: extra_script instead.
try:
    Import("pio_lib_builder")
except Exception:  # pylint: disable=broad-except
    pio_lib_builder = None

# Fallbacks used only if wl_flash_config.h cannot be parsed. The real values
# are read from the header so the two can never drift apart.
FALLBACK_DEFAULTS = {
    "WL_FLASH_BASE_ADDR": 0x08000000,
    "WL_FLASH_TOTAL_SIZE": 16384,
    "WL_FLASH_PAGE_SIZE": 64,
    "WL_PAGE_COUNT": 4,
}

DEFINE_RE = re.compile(r"^\s*#\s*define\s+(WL_[A-Z0-9_]+)\s+(.+?)\s*$")

CONFIG_HEADER = "wl_flash_config.h"


def header_candidates():
    """Places src/wl_flash_config.h can be, most specific first.

    SCons runs this through SConscript, so __file__ is undefined and the
    script cannot simply look next to itself. Both invocation modes have to
    be covered:

      - lib_deps extraScript: PlatformIO chdirs into the library directory
        and exports pio_lib_builder. The library's src/ is not yet on
        CPPPATH at this point, so CPPPATH alone is not enough.
      - pre: extra_script in the unified root build: cwd is the project
        directory and src/ sits directly under it.
    """
    out = []

    if pio_lib_builder is not None:
        try:
            out.append(os.path.join(str(pio_lib_builder.path), "src"))
        except Exception:  # pylint: disable=broad-except
            pass

    cwd = os.getcwd()
    out.append(os.path.join(cwd, "src"))
    out.append(os.path.join(os.path.dirname(cwd), "src"))

    out.append(os.path.join(env.subst("$PROJECT_DIR"), "src"))
    out.extend(str(entry) for entry in env.get("CPPPATH", []))

    return out


def find_config_header():
    for entry in header_candidates():
        try:
            path = os.path.join(env.subst(entry), CONFIG_HEADER)
        except Exception:  # pylint: disable=broad-except
            continue
        if os.path.isfile(path):
            return path
    return None


def library_defaults():
    """Read the #define defaults out of src/wl_flash_config.h.

    The header is deliberately plain — one `#define NAME literal` per
    setting — so a regex is enough and there is no second copy of these
    numbers to keep in sync.
    """
    found = {}
    header = find_config_header()

    if header:
        try:
            with open(header, "r", encoding="utf-8") as handle:
                for line in handle:
                    match = DEFINE_RE.match(line)
                    if not match:
                        continue
                    name = match.group(1)
                    if name not in FALLBACK_DEFAULTS:
                        continue
                    value = parse_c_int(match.group(2))
                    if value is not None:
                        found[name] = value
        except OSError:
            pass

    defaults = dict(FALLBACK_DEFAULTS)
    defaults.update(found)
    missing = set(FALLBACK_DEFAULTS) - set(found)
    if missing:
        print(
            "WL_FLASH: could not read %s from %s, using built-in defaults"
            % (", ".join(sorted(missing)), CONFIG_HEADER)
        )
    return defaults


def flatten_defines():
    """Collect -D settings as {name: value-as-string}.

    Two sources, because the script has to work in both contexts it is used
    from:

      - As a lib_deps extraScript it runs after the project's build_flags have
        been folded into CPPDEFINES.
      - As a `pre:` extra_script in the unified root build it runs *before*
        that happens, so CPPDEFINES holds only platform and board defines and
        the user's -D flags are still sitting in BUILD_FLAGS.

    Reading both covers either case. Values agree where they overlap.
    """
    out = {}

    def absorb(defines):
        for item in defines:
            if isinstance(item, (list, tuple)):
                if len(item) == 2:
                    out[str(item[0])] = str(item[1])
                elif len(item) == 1:
                    out[str(item[0])] = ""
            else:
                out[str(item)] = ""

    absorb(env.get("CPPDEFINES", []))

    try:
        parsed = env.ParseFlagsExtended(env.get("BUILD_FLAGS", []))
        absorb(parsed.get("CPPDEFINES", []))
    except Exception:  # pylint: disable=broad-except
        # Never let the guard itself break a build.
        pass

    return out


def parse_c_int(text):
    """Parse a C integer literal: decimal or 0x, with U/L suffixes."""
    text = str(text).strip().rstrip("uUlL")
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        return None


def physical_flash_size(board):
    """Flash size of the part, ignoring any maximum_size override.

    board_upload.maximum_size in platformio.ini is merged into the board
    manifest in memory, so board.get("upload.maximum_size") returns the
    capped value, not the physical size. The on-disk manifest is untouched.
    """
    try:
        path = board.manifest_path
        if path and os.path.isfile(path):
            import json

            with open(path, "r", encoding="utf-8") as handle:
                manifest = json.load(handle)
            size = manifest.get("upload", {}).get("maximum_size")
            if size:
                return int(size)
    except Exception:  # pylint: disable=broad-except
        pass
    return None


def fail(lines):
    raise SystemExit("\n".join([""] + lines + [""]))


def check_region():  # pylint: disable=too-many-return-statements
    board = env.BoardConfig()
    defines = flatten_defines()
    lib_defaults = library_defaults()

    values = {}
    for name, fallback in lib_defaults.items():
        if name in defines:
            parsed = parse_c_int(defines[name])
            if parsed is None:
                print(
                    "WL_FLASH: cannot parse -D%s=%s, skipping region check"
                    % (name, defines[name])
                )
                return
            values[name] = parsed
        else:
            values[name] = fallback

    base = values["WL_FLASH_BASE_ADDR"]
    page_size = values["WL_FLASH_PAGE_SIZE"]
    page_count = values["WL_PAGE_COUNT"]
    lib_flash_size = values["WL_FLASH_TOTAL_SIZE"]

    region_size = page_count * page_size

    # Where the library will actually put the region.
    if "WL_REGION_END" in defines:
        region_end = parse_c_int(defines["WL_REGION_END"])
        if region_end is None:
            print("WL_FLASH: cannot parse -DWL_REGION_END, skipping region check")
            return
        region_end_is_explicit = True
    else:
        region_end = base + lib_flash_size
        region_end_is_explicit = False

    physical = physical_flash_size(board)
    if not physical:
        print("WL_FLASH: board Flash size unknown, skipping region check")
        return

    if region_size >= physical:
        fail(
            [
                "WL_FLASH: the EEPROM region does not fit in Flash.",
                "",
                "  Flash size    : %d bytes" % physical,
                "  Region wanted : %d bytes (WL_PAGE_COUNT=%d x %d)"
                % (region_size, page_count, page_size),
                "",
                "Lower WL_PAGE_COUNT.",
            ]
        )

    # A region deliberately placed somewhere other than the end of Flash is
    # the caller's business to carve out; there is nothing to verify.
    if region_end_is_explicit and region_end != base + physical:
        print(
            "WL_FLASH: WL_REGION_END=0x%08X is not the end of Flash; "
            "reservation not verified." % region_end
        )
        return

    # The library thinks Flash ends where it does not: the region lands in
    # the middle of the address space, on top of program code.
    if region_end != base + physical:
        fail(
            [
                "WL_FLASH: the EEPROM region is not at the end of Flash.",
                "",
                "  Board Flash        : %d bytes, ending at 0x%08X"
                % (physical, base + physical),
                "  Library assumes    : %d bytes, ending at 0x%08X"
                % (lib_flash_size, region_end),
                "  Region would be at : 0x%08X..0x%08X"
                % (region_end - region_size, region_end),
                "",
                "That range is inside the program area, so records and code would",
                "overwrite each other. Tell the library the real Flash size:",
                "",
                "  build_flags = -DWL_FLASH_TOTAL_SIZE=%d" % physical,
                "",
                "(or set -DWL_REGION_END explicitly to place the region elsewhere)",
            ]
        )

    required = physical - region_size
    configured = int(board.get("upload.maximum_size", physical))

    if configured > required:
        fail(
            [
                "WL_FLASH: the EEPROM region is not excluded from the firmware image.",
                "",
                "  Flash size           : %d bytes" % physical,
                "  EEPROM region        : %d bytes (WL_PAGE_COUNT=%d x %d)"
                % (region_size, page_count, page_size),
                "  Region address range : 0x%08X..0x%08X"
                % (region_end - region_size, region_end),
                "  maximum_size is      : %d bytes" % configured,
                "  maximum_size must be : %d bytes" % required,
                "",
                "Without this cap the linker may place code inside the EEPROM region,",
                "and the first record write would erase it. Add to platformio.ini:",
                "",
                "  board_upload.maximum_size = %d" % required,
            ]
        )

    if configured < required:
        print(
            "WL_FLASH: maximum_size=%d holds back %d bytes, more than the %d the "
            "EEPROM region needs."
            % (configured, physical - configured, region_size)
        )
    else:
        print(
            "WL_FLASH: EEPROM region 0x%08X..0x%08X (%d bytes) reserved."
            % (region_end - region_size, region_end, region_size)
        )


check_region()
