#!/usr/bin/env python3
"""Focused common-payload/catalog checks; run only under admitted APP guards."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import runpy
import shutil
import subprocess
import time

REPO = Path(__file__).resolve().parents[1]
H = runpy.run_path(str(REPO / "scripts/test-native-vsh.py"), run_name="app_payload_helpers")
run, QMP, png, require, sha = (H[n] for n in ("run", "QMP", "png", "require", "sha"))


def boot(out, firmware, iso, disk):
    folder = out / firmware
    folder.mkdir()
    serial = folder / "serial.log"
    socket = folder / "qmp"
    args = ["qemu-system-x86_64", "-machine", "q35", "-accel", "tcg",
            "-m", "128M", "-cdrom", str(iso), "-boot", "d", "-display", "none",
            "-nic", "none", "-serial", "file:" + str(serial),
            "-qmp", "unix:" + str(socket) + ",server=on,wait=off",
            "-no-reboot", "-no-shutdown",
            "-drive", "file=" + str(disk) + ",if=none,id=vs0,format=raw,readonly=on",
            "-device", "virtio-blk-pci,drive=vs0,disable-legacy=on"]
    if firmware == "uefi":
        variables = folder / "OVMF_VARS_4M.fd"
        shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd", variables)
        args += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                 "-drive", "if=pflash,format=raw,unit=1,file=" + str(variables)]
    (folder / "command.json").write_text(json.dumps(args, indent=2) + "\n")
    q = None
    started = time.monotonic()
    cases = []
    with (folder / "qemu.log").open("x") as log:
        proc = subprocess.Popen(args, cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
        try:
            def data():
                require(proc.poll() is None, "QEMU exited")
                value = serial.read_bytes() if serial.exists() else b""
                require(not any(s in value for s in
                                (b"panic:", b"boot: check failed", b"boot: FAILED",
                                 b"catalog: rejected", b"native: vsh FAIL")), "guest failure")
                return value

            def wait(predicate, seconds=8):
                deadline = time.monotonic() + seconds
                while True:
                    value = data()
                    if predicate(value):
                        return value
                    require(time.monotonic() < deadline, "guest marker deadline")
                    time.sleep(0.05)

            initial = wait(lambda d: b"user[vsh]: VOS> " in d, 45)
            startup = time.monotonic() - started
            require(b"catalog: ready 3 entries, 392 bytes; all executable source pages reserved" in initial,
                    "catalog boot validation")
            for name, identity, abi, policy in [("vsh", 4097, 3, 1), ("hello", 4098, 0, 2),
                                                 ("rixie", 4099, 3, 3)]:
                require(("catalog: %s id=%d ABI=%d policy=%d size=" %
                         (name, identity, abi, policy)).encode() in initial, "catalog identity " + name)
            require(initial.index(b"catalog: ready") < initial.index(b"proc[hello]: exited"),
                    "applications ran before catalog validation")
            require(b"proc[hello]: exited status 0, all frames reclaimed" in initial, "ABI v0 hello")
            require(b"user[rixie]" not in initial, "cataloging launched Rixie")
            q = QMP(socket)

            def prompts(value):
                return re.findall(rb"(?m)^user\[vsh\]: (VOS>[^\r\n]* )\r?$", value)

            def text(value):
                codes = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
                for c in value:
                    if c.isupper():
                        q.key("shift", True)
                        q.tap(c.lower())
                        q.key("shift", False)
                    else:
                        q.tap(codes.get(c, c))

            def command(value, expected, prompt="VOS> ", erase=0):
                before = data()
                count = len(prompts(before))
                if erase:
                    q.tap("backspace")
                text(value)
                for _ in range(erase):
                    q.tap("backspace")
                q.tap("ret")
                after = wait(lambda d: len(prompts(d)) > count)
                require(expected.encode() in after[len(before):], "command reply " + value)
                require(prompts(after)[-1].decode() == prompt, "prompt " + value)
                cases.append(dict(command=value, expected=expected, prompt=prompt, passed=True))

            command("help", "Available commands: help, clear, echo, pwd, ls, cd, cat")
            command("echo common payload shell", "common payload shell")
            command("pwd", "/")
            command("ls", "docs")
            command("cd docs", "", "VOS>docs> ")
            command("cat welcome.txt", "Shared payload: real read-only filesystem.", "VOS>docs> ")
            command("cd /", "")
            command("echo catalogx", "catalog", erase=1)
            command("apps", "vsh: unknown command")
            command("rixie", "vsh: unknown command")
            q.execute("screendump", {"filename": str(folder / "interactive.ppm")})
            screenshot = png(folder / "interactive.ppm", folder / "interactive.png")
            text("exit")
            q.tap("ret")
            final = wait(lambda d: b"native: vsh PASS, process reclaimed" in d)
            require(b"user[rixie]" not in final, "runtime app launch outside Increment 1")
            deadline = time.monotonic() + 5
            while True:
                regs = q.execute("human-monitor-command", {"command-line": "info registers"})
                if "HLT=1" in regs:
                    break
                require(time.monotonic() < deadline, "kernel did not halt after shell cleanup")
                time.sleep(0.05)
            (folder / "registers.txt").write_text(regs)
            return dict(passed=True, firmware=firmware, catalog_entries=3, catalog_bytes=392,
                        hello_ABI_v0=True, vsh_ABI_v3=True, shell_cases=cases,
                        process_reclaimed=True, halted=True, rixie_not_executed=True,
                        no_runtime_launch_API=True, no_network=True, screenshot=screenshot,
                        startup_to_prompt_seconds=startup, elapsed_seconds=time.monotonic()-started)
        finally:
            if q:
                q.close()
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    phase = Path(os.environ.get("APP_PHASE_ROOT", "")).resolve()
    require(phase.is_relative_to(REPO / "build") and (phase / "APP-BUDGET.json").is_file(),
            "APP controller budget required")
    require(out.parent == phase and out.name in ("attempt-1", "attempt-2") and not out.exists(),
            "fresh admitted attempt required")
    require(json.loads((phase / "APP-BUDGET.json").read_text())["phase_limit_bytes"] == 160*1024**2,
            "approved APP budget")
    out.mkdir()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (64*1024**2, 64*1024**2))
    result = dict(scope="Common native payload and private kernel catalog only", passed=False)
    try:
        source_names = ["Makefile", "arch/x86_64/arch.mk", "arch/x86_64/kernel.c",
                        "arch/x86_64/app_catalog.c", "arch/x86_64/app_catalog.h",
                        "arch/x86_64/app_payload.c", "arch/x86_64/app_payload.h",
                        "arch/x86_64/grub-apps.cfg", "scripts/test-app-payload.py",
                        "tests/host/app-catalog.c"]
        result["source_sha256"] = {n: sha(REPO/n) for n in source_names}
        result["host_load_before"] = os.getloadavg()
        result["compiler"] = subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0]
        result["QEMU"] = subprocess.check_output(["qemu-system-x86_64", "--version"], text=True).splitlines()[0]
        rust = os.environ["RUSTC"]
        result["rustc"] = subprocess.check_output([rust, "--version"], text=True).strip()
        image = out / "image"
        user = image / "user"
        run(["make", "--no-print-directory", "ARCH=x86_64", "USER_BUILD=" + str(user),
             "RUST_APP=rixie", "RUSTC=" + rust, "user", "user-vsh", "user-rust"], out / "user-build.log")
        expected = {
            "vsh": json.loads((REPO/"docs/results/n4-prompt/evidence.json").read_text())["local_products"]["user/vsh.elf"],
            "rixie": json.loads((REPO/"docs/results/n4-rixie/evidence.json").read_text())["result"]["rixie_ELF_sha256"],
            "hello": json.loads((REPO/"docs/results/n3-evidence.json").read_text())["protected_binary_sha256"]["hello"]}
        products = {}
        for name in ("vsh", "hello", "rixie"):
            elf = user / (name + ".elf")
            run(["scripts/check-user-elf.sh", str(elf)], out / (name + "-elf.log"))
            products[name] = sha(elf)
            require(products[name] == expected[name], "accepted application ELF changed " + name)
        result["accepted_application_sha256"] = products
        cc = ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Iarch/x86_64"]
        sources = ["tests/host/app-catalog.c", "arch/x86_64/app_catalog.c", "arch/x86_64/elf.c"]
        run(cc + ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] +
            sources + ["-o", str(out/"catalog-host")], out/"host-build.log")
        elfs = [str(user/(n+".elf")) for n in ("vsh", "hello", "rixie")]
        run([str(out/"catalog-host")] + elfs, out/"host-test.log", 30)
        run(cc + ["-O2"] + sources + ["-o", str(out/"catalog-release")], out/"release-build.log")
        run([str(out/"catalog-release")] + elfs + ["--bench"], out/"release-benchmark.json", 30)
        benchmark = json.loads((out/"release-benchmark.json").read_text())
        benchmark["median_ns_per_three_entry_build"] = sorted(benchmark["sample_total_ns"])[3]/10000
        result["optimized_host_catalog_baseline"] = benchmark
        empty = subprocess.run(["make", "--no-print-directory", "-n", "apps-iso", "APP_BUILD="],
                               cwd=REPO, capture_output=True, text=True)
        require(empty.returncode == 2 and "APP_BUILD must be nonempty" in empty.stderr,
                "empty common output path accepted")
        (out/"build-path-guard.log").write_text(empty.stderr)
        run(["make", "--no-print-directory", "apps-uefi-iso", "APP_BUILD="+str(image),
             "RUSTC="+rust], out/"image-build.log", 120)
        payloads = {}
        for firmware, iso_name in (("bios", "vos.iso"), ("uefi", "vos-uefi.iso")):
            extract = out / ("extract-" + firmware)
            extract.mkdir()
            iso = image/"kernel"/iso_name
            payload = {}
            for name in ("vos.bin", "vsh.elf", "hello.elf", "rixie.elf"):
                run(["xorriso", "-osirrox", "on", "-indev", str(iso), "-extract",
                     "/boot/"+name, str(extract/name)], out/(firmware+"-extract-"+name+".log"))
                payload[name] = sha(extract/name)
            run(["xorriso", "-osirrox", "on", "-indev", str(iso), "-extract",
                 "/boot/grub/grub.cfg", str(extract/"grub.cfg")], out/(firmware+"-extract-config.log"))
            config = (extract/"grub.cfg").read_text()
            boots = [line.strip() for line in config.splitlines()
                     if line.strip().startswith(("multiboot2 ", "module2 "))]
            require(boots == [line.strip() for line in (REPO/"arch/x86_64/grub-apps.cfg").read_text().splitlines()
                              if line.strip().startswith(("multiboot2 ", "module2 "))],
                    "firmware packaging changed application/bootstrap selection")
            for name, value in products.items():
                require(payload[name+".elf"] == value, "ISO payload identity "+firmware+" "+name)
            require(payload["vos.bin"] == sha(image/"kernel/vos.bin"), "ISO kernel identity")
            payloads[firmware] = payload
        require(payloads["bios"] == payloads["uefi"], "BIOS/UEFI payload mismatch")
        result["extracted_payload_sha256"] = payloads
        source = out/"fixture"
        (source/"shell/docs").mkdir(parents=True)
        (source/"shell/docs/welcome.txt").write_text("Shared payload: real read-only filesystem.\n")
        disk = out/"disk.img"
        with disk.open("xb") as stream:
            stream.truncate(8*1024**2)
        run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "1024", "-I", "128", "-O", "none",
             "-d", str(source), str(disk)], out/"fixture-build.log")
        run(["e2fsck", "-fn", str(disk)], out/"fixture-check.log")
        os.chmod(disk, 0o444)
        disk_hash = sha(disk)
        result["firmware"] = {}
        for firmware, iso in (("bios", "vos.iso"), ("uefi", "vos-uefi.iso")):
            result["firmware"][firmware] = boot(out, firmware, image/"kernel"/iso, disk)
        require(sha(disk) == disk_hash, "read-only filesystem fixture changed")
        result["disk_sha256"] = disk_hash
        result["host_checks"] = (out/"host-test.log").read_text().strip()
        require(all(sha(REPO/n) == h for n, h in result["source_sha256"].items()), "tested source changed")
        result.update(passed=True, ABI_v0_v3_unchanged=True,
                      no_process_lifecycle_changes=True, no_execution_capability_or_syscalls=True,
                      no_console_handoff=True, BIOS_UEFI_same_application_payload=True)
    except BaseException as exc:
        result["error"] = str(exc)
        raise
    finally:
        (out/"result.json").write_text(json.dumps(result, indent=2)+"\n")
        entries = [dict(path=str(p.relative_to(out)), size=p.stat().st_size, sha256=sha(p))
                   for p in sorted(out.rglob("*")) if p.is_file()]
        (out/"manifest.json").write_text(json.dumps(entries, indent=2)+"\n")
        print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
