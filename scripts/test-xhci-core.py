#!/usr/bin/env python3
"""USB.2-A portable Rust checks; no kernel boot, hardware adapter or packages.

Run with --rustc /path/to/rustc --out build/usb2a-check (a fresh path).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
CORE = Path("kernel/rust/usb/xhci")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rustc", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / "build"):
        parser.error("fresh output below this checkout's build/ required")
    out.mkdir(parents=True)
    result = {"hardware_operations": False, "production_kernel_inputs_changed": False,
              "commands": [], "source_sha256": {}}
    for path in sorted((ROOT / CORE).glob("*.rs")):
        source = path.read_text()
        if re.search(r"\bunsafe\b|\bextern\b|\basm!|\balloc::|\bstd::", source):
            raise RuntimeError("nonportable/unsafe core source: " + str(path))
        result["source_sha256"][str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()

    def run(argv, name, expected=0):
        result["commands"].append(list(map(str, argv)))
        with (out / name).open("xb") as stream:
            process = subprocess.run(list(map(str, argv)), cwd=ROOT, stdout=stream,
                                     stderr=subprocess.STDOUT, timeout=180,
                                     env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        if process.returncode != expected:
            raise RuntimeError(f"{name}: exit {process.returncode}, expected {expected}")
        return (out / name).read_text()

    rustc = args.rustc
    common = [rustc, "--edition=2024", "-Dwarnings"]
    started = time.monotonic()
    try:
        result["compiler"] = run([rustc, "-vV"], "rustc.txt").strip()
        library = out / "libvos_xhci.rlib"
        run(common + ["--crate-name", "vos_xhci", "--crate-type=rlib", "-Copt-level=1",
                      "-Coverflow-checks=yes", "-Cdebug-assertions=yes", CORE / "lib.rs", "-o", library],
            "host-library-build.txt")
        tests = out / "tests"
        test_args = ["--test", "-Copt-level=1", "-Coverflow-checks=yes", "--extern",
                     "vos_xhci=" + str(library), "tests/rust/xhci.rs", "-o", tests]
        run(common + test_args, "tests-build.txt")
        log = run([tests, "--nocapture", "--test-threads=1"], "tests.txt")
        match = re.search(r"test result: ok\. (\d+) passed; 0 failed", log)
        if not match:
            raise RuntimeError("missing positive Rust test summary")
        result["host_tests_passed"] = int(match[1])
        for bound, phrase in [("Send", "cannot be sent between threads safely"),
                              ("Sync", "cannot be shared between threads safely")]:
            control = out / (bound.lower() + "-must-fail.rs")
            control.write_text("use vos_xhci::xhci::{controller::Controller,resources::Lease};\n"
                               "fn require<T: " + bound + ">() {}\n"
                               "fn main(){require::<Controller>();require::<Lease>();}\n")
            log = run(common + ["--crate-name", "ownership_negative", "--extern",
                                "vos_xhci=" + str(library), control, "-o", out / bound.lower()],
                      bound.lower() + "-negative.txt", expected=1)
            if phrase not in log or "E0277" not in log:
                raise RuntimeError("ownership control failed for an unrelated reason")
        result["non_Send_non_Sync_compile_controls"] = "2/2 expected compile rejections; Controller and Lease"
        # Same crate sources, same higher-half target/code model as the existing kernel.
        bare_flags = ["--crate-type=rlib", "--target=x86_64-unknown-none",
                      "-Cpanic=abort", "-Copt-level=2", "-Ctarget-cpu=x86-64", "-Ccode-model=kernel",
                      "-Crelocation-model=static", "-Cno-redzone=yes", "-Cforce-unwind-tables=no",
                      "-Clto=off", "-Cembed-bitcode=no", "-Ccodegen-units=1", "-Cdebuginfo=0"]
        run(common + ["--crate-name", "vos_xhci", *bare_flags,
                      CORE / "lib.rs", "-o", out / "libfreestanding.rlib"], "freestanding-build.txt")
        run(common + ["--crate-name", "vos_xhci_smoke", *bare_flags,
                      "--extern", "vos_xhci=" + str(out / "libfreestanding.rlib"),
                      "tests/rust/xhci_freestanding.rs", "-o", out / "libfreestanding_smoke.rlib"],
            "freestanding-concrete-build.txt")
        result["freestanding_no_std"] = True
        result["freestanding_concrete_controller_codegen"] = True
        # An isolated temporary mutation must fail the stale-handle regression.
        mutant = out / "mutant"
        shutil.copytree(ROOT / CORE, mutant)
        ring = mutant / "ring.rs"
        text = ring.read_text()
        before = "if expected != pending || epoch != self.epoch {"
        if text.count(before) != 1:
            raise RuntimeError("mutation anchor changed; review required")
        ring.write_text(text.replace(before, "if epoch != self.epoch {", 1))
        # Keep -Dwarnings: consume the deliberately disabled argument in the mutation.
        text = ring.read_text().replace("let pending = self.pending.ok_or(Error::UnexpectedEvent)?;",
                                        "let pending = self.pending.ok_or(Error::UnexpectedEvent)?; let _ = expected;", 1)
        ring.write_text(text)
        run(common + ["--crate-name", "vos_xhci", "--crate-type=rlib", mutant / "lib.rs",
                      "-o", out / "libmutant.rlib"], "mutant-build.txt")
        run(common + ["--test", "--extern", "vos_xhci=" + str(out / "libmutant.rlib"),
                      "tests/rust/xhci.rs", "-o", out / "mutant-tests"], "mutant-tests-build.txt")
        run([out / "mutant-tests", "--exact", "stale_software_ticket_rejected_after_pointer_reuse"],
            "mutant-test.txt", expected=101)
        result["stale_handle_mutation_detected"] = True
        release = out / "librelease.rlib"
        run(common + ["--crate-name", "vos_xhci", "--crate-type=rlib", "-Copt-level=2",
                      CORE / "lib.rs", "-o", release], "release-build.txt")
        run(common + ["-Copt-level=2", "--extern", "vos_xhci=" + str(release),
                      "tests/bench/xhci-core.rs", "-o", out / "bench"], "bench-build.txt")
        result["host_release_benchmark"] = json.loads(run([out / "bench"], "bench.json"))
        result["unsafe_blocks"] = 0
        result["status"] = "PASS"
    except BaseException as error:
        result["status"] = "FAIL"
        result["error"] = str(error)
        raise
    finally:
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("commands", "source_sha256")}, indent=2))


if __name__ == "__main__":
    main()
