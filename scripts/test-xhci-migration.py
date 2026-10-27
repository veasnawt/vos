#!/usr/bin/env python3
"""Independent original-C/production-Rust USB.1 differential and release cost.

Use a fresh --out below build/ and the existing bare-metal Rust compiler.
No QEMU, hardware access, packages or cleanup. Raw failures remain in that root.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = "95eb0e0c944277a4ed9f3ad0e8671e25d2d9f6cd"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rustc", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / "build"):
        parser.error("fresh output below this checkout build/ required")
    out.mkdir(parents=True)
    result = {"passed": False, "hardware_operations": False, "commands": []}

    def run(argv, name):
        result["commands"].append(list(map(str, argv)))
        with (out / name).open("xb") as stream:
            process = subprocess.run(list(map(str, argv)), cwd=ROOT, stdout=stream,
                                     stderr=subprocess.STDOUT, timeout=180,
                                     env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        if process.returncode:
            raise RuntimeError(f"{name}: exit {process.returncode}")
        return (out / name).read_text()

    try:
        reference = ROOT / "tests/reference/xhci-usb1.c"
        accepted = subprocess.check_output(["git", "show", BASE + ":arch/x86_64/xhci.c"], cwd=ROOT)
        if reference.read_bytes() != accepted:
            raise RuntimeError("independent C reference differs from accepted USB.1")
        result["reference_sha256"] = hashlib.sha256(accepted).hexdigest()
        build = out / "kernel"
        run(["make", "--no-print-directory", "ARCH=x86_64", "BUILD=" + str(build),
             "RUSTC=" + args.rustc, str(build / "rust/libvos_kernel.a")], "rust-build.log")
        library = build / "rust/libvos_kernel.a"
        result["library_sha256"] = hashlib.sha256(library.read_bytes()).hexdigest()
        rename = ["-Dxhci_probe=reference_xhci_probe", "-Dxhci_decode_caps=reference_xhci_decode_caps",
                  "-Dxhci_class_matches=reference_xhci_class_matches"]
        common = ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Iarch/x86_64",
                  "-ffunction-sections", "-fdata-sections", "-fno-pie"]
        for label, flags in [("checked", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
                             ("release", ["-O2"])]:
            obj = out / (label + "-reference.o")
            # Match the actual C kernel's scalar/no-red-zone contract for the
            # release comparison; do not compare SIMD-capable hosted C to Rust.
            kernel_flags = (["-ffreestanding", "-fno-builtin", "-mcmodel=kernel",
                             "-mno-red-zone", "-mgeneral-regs-only", "-fcf-protection=none",
                             "-fno-stack-protector", "-fno-asynchronous-unwind-tables"]
                            if label == "release" else [])
            run(common + flags + kernel_flags + rename + ["-c", reference, "-o", obj], label + "-reference-build.log")
            binary = out / label
            run(common + flags + ["-no-pie", "-Wl,--gc-sections", "tests/host/xhci-differential.c",
                                 "arch/x86_64/xhci.c", obj, library, "-o", binary], label + "-build.log")
            result[label + "_differential"] = run([binary], label + "-tests.log").strip()
            if label == "release":
                result["benchmark"] = json.loads(run([binary, "--bench"], "release-bench.json"))
        result["passed"] = True
    except BaseException as error:
        result["error"] = str(error)
        raise
    finally:
        result["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in [*sorted((ROOT / "kernel/rust/usb/xhci").glob("*.rs")),
                                             ROOT / "arch/x86_64/rust/xhci_discovery.rs",
                                             ROOT / "arch/x86_64/rust/kernel_probe.rs",
                                             ROOT / "arch/x86_64/xhci.c", reference,
                                             ROOT / "tests/host/xhci-differential.c", Path(__file__)]}
        (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
