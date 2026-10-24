#!/usr/bin/env python3
"""Gate B only: explicit selection, admitted media, strict serial contracts, no retries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
from storage_stress_b_host import Guard, MIB, require
from uefi_catalog import validate_iso
ORDER=("SC01","SC02","SC03","SC04","SC05","SC10","SC11")
IMPLEMENTED=("SC01","SC02")

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def selection(text):
    groups=tuple(text.split(","))
    require(groups and len(groups)==len(set(groups)) and all(g in IMPLEMENTED for g in groups),
            "unsupported, duplicate or empty group")
    require(groups==tuple(g for g in ORDER if g in groups),"out-of-order selection")
    return groups

BEGIN={
 "SC01":"storage-stress: SC01 BEGIN seed=0x4E360001 epochs=68 reads=136",
 "SC02":"storage-stress: SC02 BEGIN seed=0x4E360002 cases=64"}
END={
 "SC01":"storage-stress: SC01 PASS epochs=68 reads=136 overlaps=68 cpu=4 ipc=68 accounting=PASS",
 "SC02":"storage-stress: SC02 PASS cases=64 user=48 ready=16 slots=8 grants=2 accounting=PASS"}
NOTRUN="storage-stress: SC03 SC04 SC05 SC10 SC11 NOT RUN"

def trusted_lines(serial):
    return [x.removesuffix("\r") for x in serial.split("\n") if not x.startswith("user[")]

def validate_serial(serial, groups, transport, exit_status):
    require(exit_status==33,"wrong whole-boot exit")
    groups=selection(",".join(groups))
    lines=trusted_lines(serial)
    require(not any(re.search(r"FAILED|FAIL:|VOS-TEST: FAIL|panic:|check failed",x) for x in lines),
            "trusted guest failure")
    expected_transport="storage-stress: TRANSPORT "+transport+" PASS"
    require(lines.count(expected_transport)==1,"missing/duplicate/wrong transport")
    records=[x for x in lines if x.startswith("storage-stress:")]
    require(records and records[0]==expected_transport,"transport/selection record order")
    index=1
    for group in groups:
        require(index<len(records) and records[index]==BEGIN[group],"missing/duplicate/out-of-order group begin")
        index+=1
        if group=="SC01":
            for epoch in range(68):
                if epoch in (16,33,50,67):
                    want="storage-stress: SC01 CPU epoch="+str(epoch)+" PASS"
                    require(index<len(records) and records[index]==want,"missing/out-of-order CPU progress");index+=1
                want=("storage-stress: SC01 EPOCH "+str(epoch)+" region="+str(5*epoch%17)+
                      " lead="+str(epoch%2)+" reads=2 overlap=1 accounting=PASS")
                require(index<len(records) and records[index]==want,"incomplete epoch/schedule/accounting")
                index+=1
        else:
            for case in range(64):
                want=("storage-stress: SC02 CASE "+str(case)+" slot="+str(case//8)+
                      " grant="+str(case//4%2)+" term="+str(case%4)+
                      " errors=PASS incarnation=PASS accounting=PASS")
                require(index<len(records) and records[index]==want,"incomplete case/slot/grant/class/accounting")
                index+=1
        require(index<len(records) and records[index]==END[group],"missing/duplicate/incomplete group result")
        index+=1
    require(records[index:index+1]==[NOTRUN],"missing explicit nonexecution record");index+=1
    require(index<len(records) and re.fullmatch(r"storage-stress: CHECKS [1-9][0-9]* PASS",records[index]),
            "missing exact trusted assertion count")
    checks=int(records[index].split()[2]);index+=1
    require(records[index:]==["storage-stress: SELECTED PASS"],"unexpected execution/duplicate/out-of-order tail")
    require(lines.count("VOS-TEST: PASS")==1,"missing/duplicate whole-boot verdict")
    require(lines.index("VOS-TEST: PASS")>lines.index("storage-stress: SELECTED PASS"),"premature whole-boot verdict")
    return dict(groups=list(groups),checks=checks,SC01_reads=136 if "SC01" in groups else 0,
                SC01_epochs=68 if "SC01" in groups else 0,SC01_cpu=4 if "SC01" in groups else 0,
                SC01_ipc=68 if "SC01" in groups else 0,SC02_cases=64 if "SC02" in groups else 0,
                SC02_user=48 if "SC02" in groups else 0,SC02_ready=16 if "SC02" in groups else 0)

def configuration(groups, firmware):
    groups=selection(",".join(groups))
    modules=[("hello.elf","hello"),("stress.img","storage-stress.img"),
             ("r0.elf","stress-r0"),("r1.elf","stress-r1"),("cpu.elf","stress-cpu"),
             ("ipc0.elf","stress-ipc0"),("ipc1.elf","stress-ipc1"),("lifecycle.elf","stress-life")]
    cfg="set timeout=0\nset default=0\nmenuentry 'VOS N3.6 Gate B' {\n"
    cfg+="  multiboot2 /boot/vos.bin n36-groups="+",".join(groups)+"\n"
    for file,name in modules: cfg+="  module2 /boot/"+file+" "+name+"\n"
    cfg+="  boot\n}\n"
    return modules,cfg

def build_media(s, groups, firmware, env):
    r=s.root;key=firmware.lower()+"-"+",".join(groups).replace(",","-")
    out=r/"media"/key;require(not out.exists(),"refusing media overwrite");s.guard(40*MIB)
    stage=out/"isodir";(stage/"boot/grub").mkdir(parents=True)
    modules,cfg=configuration(groups,firmware)
    if firmware=="UEFI":cfg=(r/"inputs/uefi-head.cfg").read_text()+cfg
    (stage/"boot/grub/grub.cfg").write_text(cfg)
    s.copy(r/"build/stress/vos.bin",stage/"boot/vos.bin")
    for file,name in modules:
        source=(r/"inputs/stress.img") if file=="stress.img" else r/"build/user/hello.elf" if file=="hello.elf" else r/"build/user/storage_stress"/file
        s.copy(source,stage/"boot"/file)
    iso=out/"vos.iso"
    argv=["grub-mkrescue","-d",env["GRUB_EFI_DIR"] if firmware=="UEFI" else "/usr/lib/grub/i386-pc",
          "-o",str(iso),str(stage)]
    s.run("media-"+key,argv,extra=24*MIB,env_extra=env,timeout=120)
    return iso

class RunGuard(Guard):
    def guard(self,extra=0):
        row=super().guard(extra)
        watch=getattr(self,"watch",None)
        if watch:
            elapsed=time.monotonic()-watch["started"]
            p=watch["serial"]
            lines=trusted_lines(p.read_text(errors="replace")) if p.exists() else []
            now=time.monotonic()
            for group in watch["groups"]:
                if BEGIN[group] in lines and group not in watch["began"]:watch["began"][group]=now
                if group in watch["began"] and END[group] not in lines:
                    require(now-watch["began"][group]<240,"selected-group 240-second watchdog")
            if not watch["began"]:require(elapsed<60,"bounded boot allowance exceeded")
        return row

def run_profile(s, groups, firmware, cpu):
    r=s.root;label=firmware.lower()+"-"+cpu+"-"+",".join(groups).replace(",","-")
    s.guard(12*MIB)
    admitted=json.loads((r/"preboot.json").read_text())
    require(admitted["production_isolation"] and admitted["frozen_hashes"]==139,"production admission missing")
    for rel,want in admitted["sha256"].items():
        p=r/rel;require(p.resolve().is_relative_to(r) and digest(p)==want,"preboot provenance "+rel)
    key=firmware.lower()+"-"+",".join(groups).replace(",","-")
    iso=r/"media"/key/"vos.iso"
    require(str(iso.relative_to(r)) in admitted["sha256"],"selected media not admitted")
    env=json.loads((r/"build-env.json").read_text())
    if firmware=="UEFI":validate_iso(iso,r/("preboot-"+label),s,env)
    out=r/("run-"+label);require(not out.exists(),"refusing attempt overwrite");out.mkdir()
    argv=["qemu-system-x86_64","-machine","q35" if firmware=="UEFI" else "pc","-m","128M"]
    if cpu=="max":argv+=["-cpu","max"]
    if firmware=="UEFI":
        s.copy(r/"inputs/OVMF_VARS_4M.fd",out/"vars.fd")
        argv+=["-drive","if=pflash,format=raw,readonly=on,file="+str(r/"inputs/OVMF_CODE_4M.fd"),
               "-drive","if=pflash,format=raw,file="+str(out/"vars.fd")]
    argv+=["-cdrom",str(iso),"-boot","d","-drive",
           "file="+str(r/"inputs/stress.img")+",if=none,id=vd0,format=raw,readonly=on",
           "-device","virtio-blk-pci,drive=vd0,disable-legacy=on","-display","none","-no-reboot","-nic","none",
           "-serial","file:"+str(out/"serial.log"),"-device","isa-debug-exit,iobase=0xf4,iosize=0x04",
           "-d","cpu_reset,guest_errors","-D",str(out/"qemu.log")]
    window=r/"execution-window.json"
    if not window.exists():
        with window.open("x") as f:f.write(json.dumps(dict(started_monotonic=time.monotonic(),utc=time.time()))+"\n")
    result=dict(passed=False,firmware=firmware,machine="q35" if firmware=="UEFI" else "pc",
                cpu=cpu,memory="128M",groups=list(groups),argv=argv,group_deadline=240,boot_allowance=60)
    seconds=60+240*len(groups)
    s.watch=dict(started=time.monotonic(),serial=out/"serial.log",groups=groups,began={})
    try:
        s.run("run-"+label,["timeout","--kill-after=2",str(seconds),*argv],extra=8*MIB,timeout=seconds+5,expected=33)
        summary=validate_serial((out/"serial.log").read_text(),groups,"POLL" if firmware=="UEFI" else "IRQ",33)
        require("Triple fault" not in (out/"qemu.log").read_text(),"triple fault")
        for rel,want in admitted["sha256"].items():require(digest(r/rel)==want,"input changed during run "+rel)
        result.update(passed=True,exit_status=33,summary=summary,seconds=time.monotonic()-s.watch["started"],
                      serial_sha256=digest(out/"serial.log"))
    finally:
        s.watch=None
        with (out/"result.json").open("x") as f:f.write(json.dumps(result,indent=2)+"\n")
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("evidence",type=Path)
    p.add_argument("--groups",required=True);p.add_argument("--firmware",choices=["BIOS","UEFI"],required=True)
    p.add_argument("--cpu",choices=["default","max"],default="default")
    p.add_argument("--prepare-media",action="store_true")
    args=p.parse_args();groups=selection(args.groups);s=RunGuard(args.evidence)
    if args.prepare_media:build_media(s,groups,args.firmware,json.loads((s.root/"build-env.json").read_text()))
    else:print(json.dumps(run_profile(s,groups,args.firmware,args.cpu),indent=2))
if __name__=="__main__":main()
