#!/usr/bin/env python3
"""Focused execution authority/broker verification; admitted APP2 controller required."""
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
import struct
import time

REPO = Path(__file__).resolve().parents[1]
H = runpy.run_path(str(REPO / "scripts/test-native-vsh.py"), run_name="app_payload_helpers")
run, QMP, png, require, sha = (H[n] for n in ("run", "QMP", "png", "require", "sha"))

def authorization_digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(",",":")).encode()).hexdigest()

def admit_execution(identity,exists,authorization,expected_digest,state):
    """Pure preflight. Caller supplies measured state; this function writes nothing."""
    require(not exists,"execution identity already consumed")
    if identity in ("attempt-1","attempt-2"):
        return  # Original numbered-attempt admission remains unchanged.
    require(identity=="continuation-1","unrecognized continuation identity")
    require(isinstance(authorization,dict) and expected_digest,"missing continuation authorization")
    require(authorization_digest(authorization)==expected_digest,"forged continuation authorization")
    require(authorization["version"]==1 and authorization["identity"]==identity,"authorization identity")
    require(authorization["phase"]==state["phase"]==str(REPO/"build/app-exec-20261025T033737Z"),"phase binding")
    require(authorization["baseline"]==state["baseline"]=="57e230843f23ab6eb8e79d979fcf1d7d2f0a27c3","baseline binding")
    require(authorization["ledger_sha256"]==state["ledger_sha256"]==
            "45e97596677ec1f63b073fc5ba70ef857c02e64ac8f57076a88f6502d9c47807","original ledger binding")
    require(set(authorization["prior_manifests"])=={"attempt-1","attempt-2"} and
            authorization["prior_manifests"]==state["prior_manifests"],"historical manifest mismatch")
    require(state["stage"] in ("prepare","runtime"),"admission stage")
    wanted=authorization["initial_source_sha256"] if state["stage"]=="prepare" else authorization["runtime_source_sha256"]
    require(wanted==state["source_sha256"],"candidate source mismatch")
    require(authorization["individual_limit_bytes"]==80*1024**2,"continuation individual allowance")
    require(authorization["phase_limit_bytes"]==192*1024**2 and
            authorization["aggregate_limit_bytes"]==3360*1024**2,"unchanged ceilings")
    require(state["phase_charged_bytes"]+112*1024**2<=192*1024**2,"cumulative phase admission")
    require(state["aggregate_charged_bytes"]+112*1024**2<=3360*1024**2,"aggregate admission")
    require(state["isolation_ok"],"isolation admission")
    require(state["physical_ok"],"physical reserve admission")
    require(state["protected_ok"],"protected record admission")


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
                                 b"catalog: rejected", b"native: vsh FAIL", b"exec-probe FAIL")), "guest failure")
                return value

            def wait(predicate, seconds=8):
                deadline = time.monotonic() + seconds
                while True:
                    value = data()
                    if predicate(value):
                        return value
                    require(time.monotonic() < deadline, "guest marker deadline")
                    time.sleep(0.05)

            initial = wait(lambda d: b"exec-verify: SCREEN READY" in d, 60)
            startup = time.monotonic() - started
            markers = [
                b"enumeration and authority bounds PASS", b"normal negative fault recursive PASS",
                b"rights revocation stale cookie PASS", b"queued loaded pre-start cancellation PASS",
                b"allocation process thread loader rollback PASS",
                b"running reap publication contention PASS",
                b"repeat execution PASS; final parent cancellation follows",
                b"exec-verify: PASS legacy=4", b"PMM/process/thread restored; kernel CR3 reaping",
                b"recursive execution denied", b"one-slot contention denied"]
            for marker in markers:
                require(marker in initial, "native execution evidence missing: "+repr(marker))
            require(initial.count(b"legacy execution syscalls ENOSYS")==4,"four actual legacy ABI probes")
            require(b"must not appear" not in initial,"ungranted console write escaped")
            q = QMP(socket)
            q.execute("screendump", {"filename": str(folder / "execution.ppm")})
            screenshot = png(folder / "execution.ppm", folder / "execution.png")
            initial = wait(lambda d: b"user[vsh]: VOS> " in d, 12)
            require(b"proc[hello]: exited status 0, all frames reclaimed" in initial,"accepted ABI v0 hello")
            require(b"user[rixie]" not in initial,"Rixie unexpectedly executed")

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
            return dict(passed=True, firmware=firmware, execution_markers=[m.decode() for m in markers], legacy_ABI_probes=4,
                        hello_ABI_v0=True, vsh_ABI_v3=True, shell_cases=cases,
                        process_reclaimed=True, halted=True, rixie_not_executed=True,
                        no_shell_launch_commands=True, no_network=True, screenshot=screenshot,
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
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",required=True,type=Path)
    out=parser.parse_args().output.resolve()
    phase=Path(os.environ.get("APP2_PHASE_ROOT","")).resolve()
    require(phase.is_relative_to(REPO/"build") and (phase/"APP2-BUDGET.json").is_file(),
            "admitted APP2 controller required")
    budget=json.loads((phase/"APP2-BUDGET.json").read_text())
    require(budget["phase_limit_bytes"]==192*1024**2,"approved Inc2 phase budget")
    require(out.parent==phase,"execution output outside approved phase")
    authorization=None;expected_digest=None;state={}
    if out.name=="continuation-1":
        control=Path(os.environ["APP2_CONTROL_ROOT"]).resolve()
        authorization=json.loads((control/"CONTINUATION-AUTHORIZATION.json").read_text())
        expected_digest=os.environ.get("APP2_AUTH_SHA256")
        account=json.loads((control/"APP-ACCOUNTING.json").read_text())
        require(json.loads((control/"CONTROLLER-TESTS.json").read_text())["passed"],"controller tests required")
        state=dict(phase=str(phase),baseline=budget["accepted_main"],ledger_sha256=sha(phase/"APP2-BUDGET.json"),
                   prior_manifests={n:sha(phase/n/"manifest.json") for n in ("attempt-1","attempt-2")},
                   stage="runtime",source_sha256={n:sha(REPO/n) for n in authorization["runtime_source_sha256"]},
                   phase_charged_bytes=account["phase_charged_bytes"],
                   aggregate_charged_bytes=account["aggregate_conservative_charged_bytes"],
                   isolation_ok=True,physical_ok=True,protected_ok=True)
        # These true values are attested by the still-running pinned controller.
        require((control/"FIXTURE-DRY-RUN.json").is_file(),"approved fixture recipe inspection")
    admit_execution(out.name,out.exists(),authorization,expected_digest,state)
    out.mkdir()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    resource.setrlimit(resource.RLIMIT_FSIZE,(80*1024**2,80*1024**2))
    result=dict(scope="ABI v4 execution authority and bounded non-interactive kernel broker",passed=False)
    try:
        names=[n for n in budget["allowed_source_files"] if not n.startswith("docs/")]
        names += ["arch/x86_64/thread.c","arch/x86_64/loader.c","arch/x86_64/elf.c",
                  "arch/x86_64/app_catalog.c","arch/x86_64/app_payload.c","arch/x86_64/user_console.c",
                  "arch/x86_64/vsh_boot.c","user/rust/vos.rs","user/vsh/main.rs",
                  "user/rixie/main.rs","scripts/test-app-payload.py"]
        result["source_sha256"]={n:sha(REPO/n) for n in sorted(set(names))}
        result["compiler"]=subprocess.check_output(["gcc","--version"],text=True).splitlines()[0]
        result["QEMU"]=subprocess.check_output(["qemu-system-x86_64","--version"],text=True).splitlines()[0]
        rust=os.environ["RUSTC"]
        result["rustc"]=subprocess.check_output([rust,"--version"],text=True).strip()
        result["host_load_before"]=os.getloadavg()
        previous=phase/"attempt-1"
        earlier=json.loads((previous/"result.json").read_text())
        pins={r["path"]:r["sha256"] for r in json.loads((previous/"manifest.json").read_text())}
        for n in ("tests/host/exec.c","arch/x86_64/exec.c"):
            require(sha(REPO/n)==earlier["source_sha256"][n],"reused host source identity "+n)
        reused={}
        for n in ("broker-host","broker-release","host-build.log","host-test.log",
                  "release-build.log","release-benchmark.json"):
            require(sha(previous/n)==pins[n],"reused host artifact identity "+n)
            reused[n]=pins[n]
        require("PASS assertions=11232" in (previous/"host-test.log").read_text(),"passing sanitizer evidence")
        result["reused_host_evidence"]=dict(source=str(previous),manifest_sha256=sha(previous/"manifest.json"),
                                            artifacts=reused,not_rerun=True)
        shutil.copyfile(previous/"host-test.log",out/"host-test.log")
        shutil.copyfile(previous/"release-benchmark.json",out/"release-benchmark.json")
        result["optimized_host_baseline"]=earlier["optimized_host_baseline"]
        rust_previous=phase/"attempt-2"
        rust_result=json.loads((rust_previous/"result.json").read_text())
        rust_pins={r["path"]:r["sha256"] for r in json.loads((rust_previous/"manifest.json").read_text())}
        for n in ("user/rust/apps.rs","tests/rust/apps.rs"):
            require(sha(REPO/n)==rust_result["source_sha256"][n],"Rust evidence source identity "+n)
        for n in ("rust-host","rust-build.log","rust-test.log"):
            require(sha(rust_previous/n)==rust_pins[n],"Rust evidence artifact identity "+n)
        require("4 passed; 0 failed" in (rust_previous/"rust-test.log").read_text(),"passing Rust tests")
        shutil.copyfile(rust_previous/"rust-test.log",out/"rust-test.log")
        result["reused_Rust_evidence"]=dict(source=str(rust_previous),not_rerun=True,
            manifest_sha256=sha(rust_previous/"manifest.json"),artifacts={n:rust_pins[n] for n in ("rust-host","rust-build.log","rust-test.log")})
        image=out/"image";user=image/"user"
        run(["make","--no-print-directory","ARCH=x86_64","USER_BUILD="+str(user),
             "RUST_APP=rixie","RUSTC="+rust,"user","user-vsh","user-rust","user-exec-probe"],
            out/"user-build.log",90)
        expected={
            "vsh":json.loads((REPO/"docs/results/n4-prompt/evidence.json").read_text())["local_products"]["user/vsh.elf"],
            "rixie":json.loads((REPO/"docs/results/n4-rixie/evidence.json").read_text())["result"]["rixie_ELF_sha256"],
            "hello":json.loads((REPO/"docs/results/n3-evidence.json").read_text())["protected_binary_sha256"]["hello"]}
        products={}
        apps=["vsh","hello","rixie","exec-probe","exec-negative","exec-fault","exec-spin",
              "exec-recursive","exec-contender"]+["exec-legacy"+str(i) for i in range(4)]
        for name in apps:
            elf=user/(name+".elf")
            run(["scripts/check-user-elf.sh",str(elf)],out/(name+"-elf.log"))
            products[name]=sha(elf)
            if name in expected:require(products[name]==expected[name],"accepted ELF changed: "+name)
        result["application_sha256"]=products
        elfcheck=out/"elfcheck"
        run(["gcc","-std=gnu11","-O2","-Wall","-Wextra","-Werror","-Iarch/x86_64",
             "scripts/elfcheck.c","arch/x86_64/elf.c","-o",str(elfcheck)],out/"elfcheck-build.log")
        run([str(elfcheck),str(user/"exec-probe.elf")],out/"elfcheck.log")
        original=(user/"exec-probe.elf").read_bytes()
        result["ELF_negative_controls"]={}
        for kind in ("future-abi","writable-code"):
            data=bytearray(original)
            phoff=struct.unpack_from("<Q",data,32)[0];count=struct.unpack_from("<H",data,56)[0]
            changed=False
            for i in range(count):
                p=phoff+56*i;typ,flags=struct.unpack_from("<II",data,p)
                if kind=="future-abi" and typ==4:
                    offset=struct.unpack_from("<Q",data,p+8)[0]
                    struct.pack_into("<I",data,offset+16,5);changed=True;break
                if kind=="writable-code" and typ==1 and flags&1:
                    struct.pack_into("<I",data,p+4,7);changed=True;break
            require(changed,"negative ELF mutation applied")
            path=out/(kind+".elf");path.write_bytes(data)
            raw={}
            for label,argv in (("kernel",[str(elfcheck),str(path)]),
                               ("build",["scripts/check-user-elf.sh",str(path)])):
                r=subprocess.run(argv,cwd=REPO,capture_output=True,text=True)
                (out/(kind+"-"+label+".log")).write_text(r.stdout+r.stderr)
                require(r.returncode!=0,"real ELF mismatch not rejected "+kind+" "+label)
                raw[label]=dict(returncode=r.returncode,diagnostic=r.stdout+r.stderr)
            result["ELF_negative_controls"][kind]=raw
        empty=subprocess.run(["make","--no-print-directory","-n","exec-probe-iso","EXEC_BUILD="],
                             cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and "EXEC_BUILD must be nonempty" in empty.stderr,"empty output guard")
        (out/"build-path-guard.log").write_text(empty.stderr)
        run(["make","--no-print-directory","exec-probe-uefi-iso","EXEC_BUILD="+str(image),
             "RUSTC="+rust],out/"image-build.log",120)
        payloads={}
        for firmware,iso_name in (("bios","vos.iso"),("uefi","vos-uefi.iso")):
            extract=out/("extract-"+firmware);extract.mkdir()
            iso=image/"kernel"/iso_name;payload={}
            for name in ["vos.bin"]+[n+".elf" for n in apps]:
                run(["xorriso","-osirrox","on","-indev",str(iso),"-extract","/boot/"+name,
                     str(extract/name)],out/(firmware+"-extract-"+name+".log"))
                payload[name]=sha(extract/name)
            run(["xorriso","-osirrox","on","-indev",str(iso),"-extract","/boot/grub/grub.cfg",
                 str(extract/"grub.cfg")],out/(firmware+"-extract-config.log"))
            config=(extract/"grub.cfg").read_text()
            boots=lambda t:[x.strip() for x in t.splitlines() if x.strip().startswith(("multiboot2 ","module2 "))]
            require(boots(config)==boots((REPO/"arch/x86_64/grub-exec.cfg").read_text()),"fixture bootstrap packaging")
            for n,h in products.items():require(payload[n+".elf"]==h,"ISO executable identity")
            require(payload["vos.bin"]==sha(image/"kernel/vos.bin"),"ISO kernel identity")
            payloads[firmware]=payload
        require(payloads["bios"]==payloads["uefi"],"BIOS/UEFI payload mismatch")
        result["extracted_payload_sha256"]=payloads
        source=out/"fixture";(source/"shell/docs").mkdir(parents=True)
        (source/"shell/docs/welcome.txt").write_text("Shared payload: real read-only filesystem.\n")
        disk=out/"disk.img"
        with disk.open("xb") as stream:stream.truncate(8*1024**2)
        run(["mke2fs","-q","-F","-t","ext2","-b","1024","-I","128","-O","none",
             "-d",str(source),str(disk)],out/"fixture-build.log")
        run(["e2fsck","-fn",str(disk)],out/"fixture-check.log")
        os.chmod(disk,0o444);disk_hash=sha(disk)
        result["firmware"]={}
        for firmware,iso in (("bios","vos.iso"),("uefi","vos-uefi.iso")):
            result["firmware"][firmware]=boot(out,firmware,image/"kernel"/iso,disk)
        require(sha(disk)==disk_hash,"read-only fixture changed")
        result["disk_sha256"]=disk_hash
        result["host_checks"]=(out/"host-test.log").read_text().strip()
        result["rust_checks"]=(out/"rust-test.log").read_text().strip()
        require(all(sha(REPO/n)==h for n,h in result["source_sha256"].items()),"tested source changed")
        result.update(passed=True,ABI_v0_v3_compatible=True,no_console_handoff=True,
                      no_shell_launch_commands=True,no_network_grants=True,physical_hardware_verified=False)
    except BaseException as exc:
        result["error"]=str(exc)
        raise
    finally:
        (out/"result.json").write_text(json.dumps(result,indent=2)+"\n")
        entries=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p))
                 for p in sorted(out.rglob("*")) if p.is_file()]
        (out/"manifest.json").write_text(json.dumps(entries,indent=2)+"\n")
        print(json.dumps(result),flush=True)

if __name__=="__main__":main()
