#!/usr/bin/env python3
"""Regenerate compile_commands.json for clangd (and Serena's clangd backend).

Why this is a script and not a committed file
---------------------------------------------
clangd resolves the database entry's ``directory`` and every ``-I`` path
against the *machine's* filesystem, and it expands neither ``$VAR``/``%VAR%``
nor ``~`` inside compile_commands.json (verified against clangd 19.1.2). The
PlatformIO package directories (SDK headers, toolchain headers, the
``lib_deps`` checkout) live under the user's PlatformIO home, so a committed
database would only ever be valid on the machine that wrote it. The database
is therefore gitignored and rebuilt per machine.

Always pass exactly one environment
-----------------------------------
``pio run -t compiledb`` without ``-e`` walks *every* environment in
platformio.ini and each one overwrites the database, so the last one wins.
Building ``Proteus32-RGB`` last silently switches clangd to that env's define
set (``-DUSE_IND_RGB=1``), which changes struct layouts in ``app_cfg_t`` and
``aux_leds.h`` and mis-parses ~44 ``#if USE_IND_RGB`` sites. This script
defaults to the default environment and verifies the result.

Usage
-----
    python tools/gen_compile_db.py                  # env:Proteus32
    python tools/gen_compile_db.py --env Proteus32-RGB
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
DB_PATH = PROJECT_DIR / "compile_commands.json"
DEFAULT_ENV = "Proteus32"
# src/*.c that must appear in the database; a missing entry means the database
# is stale and clangd falls back to flagless parsing for that file.
EXPECTED_FLAGS = ("-DCH32V003", "-DWL_OB_ENABLE", "-DWL_FORMAT_VERSION=4",
                  "-DSYSCLK_FREQ_24MHz_HSI=24000000")
RGB_ONLY_FLAGS = ("-DUSE_IND_RGB=1",)


def pio_command():
    """Return the argv prefix that runs PlatformIO on this machine."""
    for exe in ("pio", "platformio"):
        found = shutil.which(exe)
        if found:
            return [found]
    return [sys.executable, "-m", "platformio"]


def run_pio(env_name):
    argv = pio_command() + ["run", "-t", "compiledb", "-e", env_name]
    print("$ " + " ".join(argv))
    result = subprocess.run(argv, cwd=str(PROJECT_DIR))
    if result.returncode != 0:
        print(f"error: 'pio run -t compiledb -e {env_name}' failed "
              f"(exit {result.returncode})", file=sys.stderr)
        return False
    return True


def load_db():
    if not DB_PATH.exists():
        print(f"error: {DB_PATH} was not created", file=sys.stderr)
        return None
    raw = DB_PATH.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        print("error: compile_commands.json has a UTF-8 BOM; clangd's JSON "
              "parser rejects it and falls back to flagless parsing "
              "(delete the file and rerun)", file=sys.stderr)
        return None
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as err:
        print(f"error: cannot parse compile_commands.json: {err}", file=sys.stderr)
        return None


def entry_path(entry):
    """Absolute path of a database entry's source file.

    PlatformIO writes `file` relative to `directory`, so the two have to be
    joined before comparing against the project layout.
    """
    path = Path(entry["file"])
    if path.is_absolute():
        return path
    return Path(entry["directory"]) / path


def check(entries, env_name):
    """Return a list of problems found in the freshly generated database."""
    problems = []

    src_dir = PROJECT_DIR / "src"
    sources = sorted(p.name for p in src_dir.glob("*.c"))
    covered = {entry_path(e).name for e in entries
               if entry_path(e).parent == src_dir}
    missing = [name for name in sources if name not in covered]
    if missing:
        problems.append(f"no database entry for src/{', src/'.join(missing)}")

    sample = next((e for e in entries if entry_path(e).parent == src_dir), None)
    if sample is None:
        problems.append("no src/*.c entries at all")
        return problems

    command = sample["command"]
    for flag in EXPECTED_FLAGS:
        if flag not in command.split():
            problems.append(f"expected {flag} in the src/*.c command line")
    for flag in RGB_ONLY_FLAGS:
        if env_name != "Proteus32-RGB" and flag in command.split():
            problems.append(f"{flag} present although {env_name} does not "
                            "define it: the database was clobbered by "
                            "another environment")

    for entry in entries:
        for token in entry["command"].split():
            if not token.startswith("-I"):
                continue
            path = token[2:]
            if not path:
                continue
            resolved = (Path(entry["directory"]) / path) if not os.path.isabs(path) \
                else Path(path)
            if not resolved.is_dir():
                problems.append(f"missing include dir: {resolved}")
                break
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-e", "--env", default=DEFAULT_ENV,
                        help=f"PlatformIO environment (default: {DEFAULT_ENV})")
    args = parser.parse_args()

    if not run_pio(args.env):
        return 1

    entries = load_db()
    if entries is None:
        return 1

    problems = check(entries, args.env)
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        if any("missing include dir" in p for p in problems):
            print("hint: run 'pio run -e " + args.env + "' once to install the "
                  "platform, the toolchain and the lib_deps (WL_FLASH) "
                  "packages, then rerun this script", file=sys.stderr)
        return 1

    print(f"ok: {DB_PATH.name} regenerated for env:{args.env} "
          f"({len(entries)} entries)")
    print("hint: restart the clangd/Serena language server to pick it up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
