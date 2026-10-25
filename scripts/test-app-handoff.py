#!/usr/bin/env python3
"""Focused Increment3 console ownership and real shell launches. Guarded fresh outputs."""
import argparse,hashlib,json,os,re,resource,runpy,shutil,struct,subprocess,time
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/"scripts/test-native-vsh.py"),run_name="handoff_helpers")
run,QMP,png,require,sha=(H[n] for n in ("run","QMP","png","require","sha"))
def capture(q,folder,name):
    path=folder/(name+".png")
    schema=q.execute("query-qmp-schema");table={x["name"]:x for x in schema}
    members=table[table["screendump"]["arg-type"]].get("members",[])
    fmt=next((x for x in members if x["name"]=="format"),None)
    choices=table[fmt["type"]] if fmt else {}
    values=choices.get("values",choices.get("members",[]))
    direct=any((x if isinstance(x,str) else x.get("name"))=="png" for x in values)
    if direct:q.execute("screendump",{"filename":str(path),"format":"png"})
    else:
        ppm=folder/(name+".ppm");q.execute("screendump",{"filename":str(ppm)})
        png(ppm,path)
    data=path.read_bytes();require(data[:8]==b"\x89PNG\r\n\x1a\n","real PNG screenshot")
    width,height=struct.unpack_from(">II",data,16)
    require(width>=640 and height>=400,"meaningful screenshot geometry")
    return dict(path=str(path),width=width,height=height,sha256=sha(path),direct_QMP_PNG=direct)

def boot(out,firmware,profile,iso,disk):
    folder=out/(profile+"-"+firmware);folder.mkdir()
    serial=folder/"serial.log";socket=folder/"qmp"
    argv=["qemu-system-x86_64","-machine","q35","-accel","tcg","-m","128M",
          "-cdrom",str(iso),"-boot","d","-display","none","-nic","none",
          "-vga","none","-device","VGA,edid=on,xres=1024,yres=768",
          "-serial","file:"+str(serial),"-qmp","unix:"+str(socket)+",server=on,wait=off",
          "-no-reboot","-no-shutdown",
          "-drive","file="+str(disk)+",if=none,id=vs0,format=raw,readonly=on",
          "-device","virtio-blk-pci,drive=vs0,disable-legacy=on"]
    if firmware=="uefi":
        variables=folder/"OVMF_VARS_4M.fd";shutil.copyfile("/usr/share/OVMF/OVMF_VARS_4M.fd",variables)
        argv+=["-drive","if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
               "-drive","if=pflash,format=raw,unit=1,file="+str(variables)]
    (folder/"command.json").write_text(json.dumps(argv,indent=2)+"\n")
    q=None;start=time.monotonic();cases=[];latencies=[]
    with (folder/"qemu.log").open("x") as log:
        proc=subprocess.Popen(argv,cwd=REPO,stdout=log,stderr=subprocess.STDOUT)
        try:
            def data():
                require(proc.poll() is None,"QEMU exited")
                value=serial.read_bytes() if serial.exists() else b""
                require(not any(x in value for x in (b"panic:",b"boot: check failed",b"boot: FAILED",
                     b"exec-probe FAIL",b"handoff-probe FAIL",b"native: vsh FAIL")),"native failure; preserve guest")
                return value
            def wait(predicate,timeout=10):
                deadline=time.monotonic()+timeout
                while True:
                    value=data()
                    if predicate(value):return value
                    require(time.monotonic()<deadline,"native marker deadline")
                    time.sleep(.02)
            def text(s):
                codes={" ":"spc","/":"slash",".":"dot","-":"minus"}
                for c in s:
                    if c.isupper():
                        q.key("shift",True);q.tap(c.lower());q.key("shift",False)
                    else:q.tap(codes.get(c,c))
            def batch(chars):
                events=[]
                codes={"\n":"ret"," ":"spc","-":"minus"}
                for c in chars:
                    key={"type":"qcode","data":codes.get(c,c)}
                    events.extend([{"type":"key","data":{"down":True,"key":key}},
                                   {"type":"key","data":{"down":False,"key":key}}])
                q.execute("input-send-event",{"events":events})
            def rixie_command(s,expected):
                before=data();count=before.count(b"user[rixie]: Rixie> ")
                text(s);q.tap("ret")
                after=wait(lambda d:d.count(b"user[rixie]: Rixie> ")>count)
                require(expected.encode() in after[len(before):],"Rixie reply "+s)
                cases.append(dict(application="rixie",command=s,expected=expected,passed=True))
            native=None
            if profile=="instrumented":
                initial=wait(lambda d:b"handoff-verify: QUEUE PRE-HANDOFF" in d,60)
                q=QMP(socket);batch("old")
                wait(lambda d:b"user[rixie]: Rixie> " in d)
                rixie_command("help","Topics: hello, help, about, questions")
                rixie_command("about","independent Rust ring-3 application")
                batch("exit\nx")
                wait(lambda d:b"user[handoff-probe]: Parent> " in d)
                text("parent-ok");q.tap("ret")
                native=wait(lambda d:b"handoff-verify: PASS cases=21" in d,40)
                require(b"exec-verify: PASS legacy=4 controls=33 submitted=23 started=15 reclaimed=18 cancelled=6 failed=8" in native,"existing broker native compatibility")
                for n in range(21):
                    require(("handoff-verify: CASE %d PASS; resources restored"%n).encode() in native,"native case "+str(n))
                require(b"handoff-probe: parent input isolation PASS" in native,"queued child input crossed restoration")
                require(b"requests=20 loaded=16 started=14 reclaimed=16 cancelled=6 failed=6 transfers=16 restores=11 abandoned=5" in native,"native lifecycle/ownership counts")
                require(b"must not appear" not in native,"ungranted output escaped")
            initial=wait(lambda d:b"user[vsh]: VOS> " in d,60 if profile=="production" else 12)
            startup=time.monotonic()-start
            require(b"catalog: vsh id=4097 ABI=4 policy=1" in initial,"real ABI4 shell")
            if q is None:q=QMP(socket)
            def prompts(d):return re.findall(rb"(?m)^user\[vsh\]: (VOS>[^\r\n]* )\r?$",d)
            def command(s,expected="",prompt="VOS> "):
                before=data();count=len(prompts(before))
                text(s);q.tap("ret");last_enter=time.monotonic()
                after=wait(lambda d:len(prompts(d))>count)
                latency=time.monotonic()-last_enter
                require(expected.encode() in after[len(before):],"shell reply "+s)
                require(prompts(after)[-1].decode()==prompt,"cwd/prompt "+s)
                cases.append(dict(application="vsh",command=s,expected=expected,prompt=prompt,passed=True))
                latencies.append(dict(command=s,after_enter_to_prompt_seconds=latency))
                return after[len(before):]
            command("help","Available commands: help, clear, echo, pwd, ls, cd, cat, apps, hello, rixie")
            apps=command("apps","hello")
            require(b"user[vsh]: rixie\r\n" in apps and b"user[vsh]: vsh\r\n" not in apps,"filtered shell catalog")
            command("echo hello world","hello world")
            command("ls","docs")
            command("cd docs","","VOS>docs> ")
            command("pwd","/docs","VOS>docs> ")
            command("hello","Hello from VOS!","VOS>docs> ")
            before=data();shell_prompts=len(prompts(before));rixie_prompts=before.count(b"user[rixie]: Rixie> ")
            text("rixie");q.tap("ret")
            wait(lambda d:d.count(b"user[rixie]: Rixie> ")>rixie_prompts)
            rixie_command("hello","Hello! I'm Rixie, at home in VOS.")
            rixie_command("help","Topics: hello, help, about, questions")
            rixie_command("about","deterministic prototype with no conversational AI model or network")
            rixie_command("explain cd","invalid paths leave it unchanged")
            rixie_command("are you online","entirely offline")
            require(len(prompts(data()))==shell_prompts,"shell consumed input while child active")
            pictures={}
            if profile=="production":pictures["rixie"]=capture(q,folder,"rixie")
            batch("exit\nx")
            after=wait(lambda d:len(prompts(d))>shell_prompts)
            require(b"Goodbye from Rixie." in after[len(before):],"actual Rixie exit")
            require(prompts(after)[-1]==b"VOS>docs> ","restored original cwd prompt")
            command("pwd","/docs","VOS>docs> ")
            command("cat welcome.txt","Unified apps: real read-only filesystem.","VOS>docs> ")
            if profile=="production":pictures["restored_shell"]=capture(q,folder,"restored-shell")
            for _ in range(2):command("hello","Hello from VOS!","VOS>docs> ")
            count=data().count(b"user[rixie]: Rixie> ");sp=len(prompts(data()))
            text("rixie");q.tap("ret");wait(lambda d:d.count(b"user[rixie]: Rixie> ")>count)
            rixie_command("hello","Hello! I'm Rixie")
            batch("exit\n");wait(lambda d:len(prompts(d))>sp)
            command("pwd","/docs","VOS>docs> ")
            command("hello extra","application arguments are not supported","VOS>docs> ")
            command("apps extra","operation failed","VOS>docs> ")
            command("missing-app","unknown command","VOS>docs> ")
            command("/hello","unknown command","VOS>docs> ")
            command("cd missing","operation failed","VOS>docs> ")
            q.tap("backspace");text("echo editingx");q.tap("backspace");q.tap("ret")
            wait(lambda d:b"user[vsh]: editing\r\n" in d)
            command("cd /")
            command("clear")
            command("echo shell restored","shell restored")
            text("exit");q.tap("ret")
            final=wait(lambda d:b"native: vsh PASS, process reclaimed" in d)
            deadline=time.monotonic()+5
            while True:
                regs=q.execute("human-monitor-command",{"command-line":"info registers"})
                if "HLT=1" in regs:break
                require(time.monotonic()<deadline,"kernel not halted after cleanup");time.sleep(.02)
            (folder/"registers.txt").write_text(regs)
            require(b"must not appear" not in final,"denied output leaked")
            return dict(passed=True,firmware=firmware,profile=profile,cases=cases,screenshots=pictures,
                        native_fault_injection=profile=="instrumented",native_cases=21 if native else 0,
                        historical_broker_native_cases=23 if native else 0,ABI4_vsh=True,ABI3_rixie=True,
                        ABI0_hello=True,explicit_console_restoration=True,input_boundary_test=True,
                        process_reclaimed=True,halted=True,startup_to_prompt_seconds=startup,
                        elapsed_seconds=time.monotonic()-start,host_observed_command_latency=latencies)
        finally:
            if q:q.close()
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument("--output",required=True,type=Path)
    out=ap.parse_args().output.resolve()
    phase=Path(os.environ.get("APP3_PHASE_ROOT","")).resolve()
    require(phase.is_relative_to(REPO/"build") and (phase/"APP3-BUDGET.json").is_file(),"APP3 controller required")
    budget=json.loads((phase/"APP3-BUDGET.json").read_text())
    require(budget["phase_limit_bytes"]==256*1024**2 and budget["attempt_limit_bytes"]==112*1024**2,"approved bounds")
    require(out.parent==phase and out.name in ("attempt-1","attempt-2") and not out.exists(),"fresh admitted attempt")
    out.mkdir()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(112*1024**2,112*1024**2))
    result=dict(passed=False,scope="Exclusive console handoff and native vsh launching")
    try:
        names=[n for n in budget["allowed_source_files"] if not n.startswith("docs/")]
        names=[n for n in names if (REPO/n).is_file()]
        names+=["arch/x86_64/process.c","arch/x86_64/process.h","arch/x86_64/thread.c",
                "arch/x86_64/input_console.c","arch/x86_64/loader.c","arch/x86_64/elf.c",
                "arch/x86_64/app_payload.c","user/rust/apps.rs","user/rust/vos.rs",
                "user/rixie/main.rs","user/rixie/dialog.rs"]
        result["source_sha256"]={n:sha(REPO/n) for n in sorted(set(names))}
        rust=os.environ["RUSTC"]
        result["rustc"]=subprocess.check_output([rust,"--version"],text=True).strip()
        result["compiler"]=subprocess.check_output(["gcc","--version"],text=True).splitlines()[0]
        result["QEMU"]=subprocess.check_output(["qemu-system-x86_64","--version"],text=True).splitlines()[0]
        result["host_load_before"]=os.getloadavg()
        cc=["gcc","-std=gnu11","-Wall","-Wextra","-Werror","-Iarch/x86_64","-ffunction-sections","-fdata-sections","-Wl,--gc-sections"]
        san=["-O1","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer"]
        broker=["tests/host/exec.c","arch/x86_64/exec.c"]
        console=["tests/host/user-console-handoff.c","arch/x86_64/user_console.c","arch/x86_64/keyboard.c","arch/x86_64/input_console.c","arch/x86_64/syscall.c"]
        run(cc+san+["-DVOS_EXEC_HOST"]+broker+["-o",str(out/"broker-host")],out/"broker-host-build.log")
        run([str(out/"broker-host")],out/"broker-host-test.log",20)
        run(cc+san+["-DVOS_CONSOLE_HOST"]+console+["-o",str(out/"console-host")],out/"console-host-build.log")
        run([str(out/"console-host")],out/"console-host-test.log",20)
        run(cc+["-O2","-DVOS_CONSOLE_HOST"]+console+["-o",str(out/"console-release")],out/"console-release-build.log")
        samples=[]
        for n in range(7):
            start=time.perf_counter_ns()
            run([str(out/"console-release")],out/("console-release-"+str(n)+".log"),20)
            samples.append(time.perf_counter_ns()-start)
        result["optimized_host_baseline"]=dict(scope="actual console/keyboard/editor with modeled I/O and scheduler; includes process startup and assertions",samples_ns=samples,median_ns=sorted(samples)[3],not_native_hardware_latency=True)
        run([rust,"--edition=2024","--test","tests/rust/apps.rs","-o",str(out/"rust-host")],out/"rust-host-build.log")
        run([str(out/"rust-host"),"--test-threads=1"],out/"rust-host-test.log")
        image=out/"image";verify=out/"verify";user=image/"user"
        # All output paths are fresh. One common user build is shared by both kernel profiles.
        dry=subprocess.run(["make","--no-print-directory","-n","ARCH=x86_64","USER_BUILD="+str(user),
                            "RUST_APP=rixie","RUSTC="+rust,"user","user-vsh","user-rust","user-exec-probe","user-handoff-probe"],
                           cwd=REPO,capture_output=True,text=True)
        require(dry.returncode==0,"build dry run")
        (out/"user-build-dry-run.log").write_text(dry.stdout+dry.stderr)
        require("-o "+str(user/"vsh.elf") in dry.stdout and "-o "+str(user/"exec-negative.elf") in dry.stdout,"nonempty linker targets")
        run(["make","--no-print-directory","ARCH=x86_64","USER_BUILD="+str(user),"RUST_APP=rixie","RUSTC="+rust,
             "user","user-vsh","user-rust","user-exec-probe","user-handoff-probe"],out/"user-build.log",120)
        apps=["vsh","hello","rixie","exec-probe","handoff-probe","exec-negative","exec-fault",
              "exec-spin","exec-recursive","exec-contender"]+["exec-legacy"+str(i) for i in range(4)]
        products={}
        for name in apps:
            run(["scripts/check-user-elf.sh",str(user/(name+".elf"))],out/(name+"-elf.log"))
            products[name]=sha(user/(name+".elf"))
        accepted=json.loads((REPO/"docs/results/app-exec/result.json").read_text())["application_sha256"]
        for name in ("hello","rixie"):require(products[name]==accepted[name],"protected application identity "+name)
        require(products["vsh"]!=accepted["vsh"],"vsh intentionally upgraded ABI4")
        result["application_sha256"]=products
        catalog=["tests/host/app-catalog.c","arch/x86_64/app_catalog.c","arch/x86_64/elf.c"]
        run(cc+san+catalog+["-o",str(out/"catalog-host")],out/"catalog-build.log")
        run([str(out/"catalog-host")]+[str(user/(n+".elf")) for n in ("vsh","hello","rixie")],out/"catalog-test.log",20)
        legacy=REPO/"build/app-exec-20261025T033737Z/continuation-1/image/user"
        run([str(out/"catalog-host")]+[str(legacy/(n+".elf")) for n in ("vsh","hello","rixie")],out/"catalog-legacy-test.log",20)
        run(["make","--no-print-directory","apps-uefi-iso","APP_BUILD="+str(image),"RUSTC="+rust],out/"production-build.log",120)
        run(["make","--no-print-directory","handoff-uefi-iso","HANDOFF_BUILD="+str(verify),
             "HANDOFF_USER_BUILD="+str(user),"RUSTC="+rust],out/"instrumented-build.log",120)
        result["images"]={}
        for profile,where in (("production",image),("instrumented",verify)):
            expected_apps=["vsh","hello","rixie"] if profile=="production" else apps
            expected={"vos.bin":sha(where/"kernel/vos.bin"),**{n+".elf":products[n] for n in expected_apps}}
            result["images"][profile]={}
            for fw,iso in (("bios","vos.iso"),("uefi","vos-uefi.iso")):
                extract=out/("extract-"+profile+"-"+fw);extract.mkdir()
                for name in expected:
                    run(["xorriso","-osirrox","on","-indev",str(where/"kernel"/iso),"-extract",
                         "/boot/"+name,str(extract/name)],out/(profile+"-"+fw+"-"+name+"-extract.log"))
                    require(sha(extract/name)==expected[name],"extracted firmware payload "+profile+" "+fw+" "+name)
                result["images"][profile][fw]=dict(ISO_sha256=sha(where/"kernel"/iso),payload_sha256=expected)
        for name in ("exec","user_console"):
            result[name+"_kernel_object_nm"]=subprocess.check_output(["nm","-S","--size-sort",str(image/"kernel"/(name+".o"))],text=True)
        source=out/"fixture";(source/"shell/docs").mkdir(parents=True)
        (source/"shell/docs/welcome.txt").write_text("Unified apps: real read-only filesystem.\n")
        disk=out/"disk.img"
        with disk.open("xb") as f:f.truncate(8*1024**2)
        run(["mke2fs","-q","-F","-t","ext2","-b","1024","-I","128","-O","none","-d",str(source),str(disk)],out/"fixture-build.log")
        run(["e2fsck","-fn",str(disk)],out/"fixture-check.log")
        os.chmod(disk,0o444);disk_hash=sha(disk)
        result["firmware"]={}
        for fw,iso in (("bios","vos.iso"),("uefi","vos-uefi.iso")):
            result["firmware"][fw]={}
            for profile,where in (("instrumented",verify),("production",image)):
                result["firmware"][fw][profile]=boot(out,fw,profile,where/"kernel"/iso,disk)
        require(sha(disk)==disk_hash,"read-only fixture unchanged")
        result["disk_sha256"]=disk_hash
        require(all(sha(REPO/n)==h for n,h in result["source_sha256"].items()),"tested source changed")
        result["host_results"]={n:(out/(n+"-test.log")).read_text() for n in ("broker-host","console-host","rust-host","catalog","catalog-legacy")}
        result["peak_host_child_RSS_KiB"]=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        result.update(passed=True,physical_hardware_verified=False,no_network_grants=True,
                      no_background_or_recursive_launch=True,no_filesystem_executable_loader=True)
    except BaseException as ex:
        result["error"]=str(ex);raise
    finally:
        (out/"result.json").write_text(json.dumps(result,indent=2)+"\n")
        entries=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob("*")) if p.is_file()]
        (out/"manifest.json").write_text(json.dumps(entries,indent=2)+"\n")
        print(json.dumps(result),flush=True)
if __name__=="__main__":main()
