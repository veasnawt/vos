#!/usr/bin/env python3
"""Non-destructive regression for the root Makefile's fail-closed clean target."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

MESSAGE = ("make clean is disabled: build/ contains protected evidence and controller "
           "dependencies. Selective cleanup requires separate authorization.")
RECIPE = "clean:\n\t@printf '%s\\n' '" + MESSAGE + "' >&2\n\t@exit 1\n"

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

def inspect(text):
    # Accept only this literal, prerequisite-free refusal; never execute rejected text.
    heads = list(re.finditer(r"(?m)^clean\s*:", text))
    require(len(heads) == 1, "exactly one clean rule required")
    start = heads[0].start()
    lines = text[start:].splitlines(keepends=True)
    body = lines[0]
    for line in lines[1:]:
        if not line.startswith("\t"):
            break
        body += line
    require(body == RECIPE, "clean must contain only the literal diagnostic and failure")
    require(any(line.startswith(".PHONY:") and "clean" in line.split()[1:]
                for line in text.splitlines()), "clean must remain phony")
    return body

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--makefile", type=Path, default=Path(__file__).resolve().parents[1] / "Makefile")
    parser.add_argument("--scratch", type=Path, required=True,
                        help="fresh admitted directory; evidence is retained, never cleaned")
    args = parser.parse_args()
    text = args.makefile.read_text(encoding="utf-8")
    recipe = inspect(text)
    rejected = [
        text.replace(RECIPE, "clean:\n\trm -rf build\n"),
        text.replace(RECIPE, RECIPE + "\trm -rf build\n"),
        text.replace("clean:\n", "clean: dangerous\n", 1),
        text.replace("\t@exit 1\n", "\t@exit 0\n", 1),
        text + "\nclean:\n\t@exit 1\n",
        text.replace(".PHONY:", "# .PHONY:"),
    ]
    for candidate in rejected:
        try:
            inspect(candidate)
        except RuntimeError:
            continue
        raise RuntimeError("unsafe negative control was accepted")
    scratch = args.scratch.resolve()
    require(scratch.is_dir() and not args.scratch.is_symlink()
            and not list(scratch.iterdir()), "scratch must be admitted and empty")
    marker = scratch / "protected-evidence.keep"
    with marker.open("xb") as stream:
        stream.write(b"D1-B1 retained non-destructive fixture\n")
    before = hashlib.sha256(marker.read_bytes()).hexdigest()
    env = os.environ.copy()
    for name in ("MAKEFLAGS", "MFLAGS", "MAKEFILES", "GNUMAKEFLAGS"):
        env.pop(name, None)
    fixture = ".PHONY: clean\n" + recipe
    result = subprocess.run(["make", "--no-print-directory", "-rR", "-f", "-", "clean"],
                            input=fixture, text=True, cwd=scratch, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
    require(result.returncode == 2, "make must report the intentional recipe failure")
    require(MESSAGE in result.stderr, "clear diagnostic required")
    require(list(scratch.iterdir()) == [marker]
            and hashlib.sha256(marker.read_bytes()).hexdigest() == before,
            "fixture contents must remain unchanged")
    print(json.dumps(dict(passed=True, unsafe_controls_rejected=len(rejected),
                          fixture_exit=result.returncode, marker_sha256=before,
                          inspection_before_execution=True)))
if __name__ == "__main__":
    main()
