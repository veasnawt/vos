#!/usr/bin/env python3
"""Gate D only: explicit selection, admitted media, strict SC10/SC11 serial contracts, no retries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
from storage_stress_b_host import Guard, MIB, require
from uefi_catalog import validate_iso
ORDER=("SC01","SC02","SC03","SC04","SC05","SC10","SC11")
IMPLEMENTED=("SC10","SC11")
TRANSPORTS=("IRQ","POLL")
LIMIT=512                                   # IO_SCOPE_LOGICAL_LIMIT
DEPTHS=(8,56,57,64)
PASSES=("cold","warm","reopened","independent")
PASS_COST=1408                              # 13 + 199*7 + 2 per /many pass (fixture model)
# /scope/huge probes: cookie, modelled cost, result (EAGAIN when the cost exceeds the budget), entry
PROBES=((0,1267,"EAGAIN",None),(1024,1261,"EAGAIN",None),(351232,513,"EAGAIN",None),(352256,510,"ENTRY","tail"),
        (522240,12,"ENTRY","tail"),(523264,9,"ENTRY","tail"),(524288,2,"EOF",None),(524289,2,"EINVAL",None),
        (523265,7,"EINVAL",None),(24,5,"EINVAL",None))
CHECKS=(("closed-handle","EBADHANDLE"),("file-handle","ENOTDIR"),("record-size","EINVAL"))

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def selection(text):
    groups=tuple(text.split(","))
    require(groups and len(groups)==len(set(groups)) and all(g in IMPLEMENTED for g in groups),
            "unsupported, duplicate or empty group")
    require(groups==tuple(g for g in ORDER if g in groups),"out-of-order selection")
    return groups

def transport_of(firmware):
    return "POLL" if firmware=="UEFI" else "IRQ"

BEGIN={
 "SC10":"storage-stress: SC10 BEGIN seed=0x4E36000A cases=14",
 "SC11":"storage-stress: SC11 BEGIN seed=0x4E36000B calls=814 checks=4"}
END={
 "SC10":"storage-stress: SC10 PASS cases=14 accounting=PASS",
 "SC11":"storage-stress: SC11 PASS calls=814 checks=4 accounting=PASS"}

def orphan(transport):
    require(transport in TRANSPORTS,"unknown transport")
    return int(transport=="IRQ")              # a killed interrupt-mode request is abandoned; polling finishes it

def sc10_cases(transport):
    """14 cases: depths 8/56/57/64 cold and warm (cost 2+9d), three exhaust/recover cycles, two kills, isolation."""
    n=0
    for d in DEPTHS:
        cost=2+9*d
        for cache in ("cold","warm"):
            yield dict(case=n,orphan=0),("storage-stress: SC10 CASE %d kind=depth depth=%d cache=%s cost=%d consumed=%d "
                                          "result=%s reads=%s accounting=PASS" % (n,d,cache,cost,min(cost,LIMIT),
                                          "OPEN" if cost<=LIMIT else "EAGAIN","DEVICE" if cache=="cold" else "NONE"))
            n+=1
    for k in (1,2,3):
        yield dict(case=n,orphan=0),"storage-stress: SC10 CASE %d kind=recovery cycle=%d exhausted=512 recovered=506 accounting=PASS" % (n,k)
        n+=1
    for d in (56,64):
        o=orphan(transport)
        yield dict(case=n,orphan=o),("storage-stress: SC10 CASE %d kind=kill depth=%d orphan=%d kill=PASS publication=NONE "
                                      "accounting=PASS" % (n,d,o))
        n+=1
    yield dict(case=n,orphan=0),"storage-stress: SC10 CASE 13 kind=isolation exhausted=512 independent=74 accounting=PASS"

def sc11_records(transport):
    """Four /many passes (804 calls) and ten /scope/huge probes (814 enumeration calls), then four separate checks."""
    for i,mode in enumerate(PASSES,1):
        yield dict(case="pass %d" % i,kind="many"),("storage-stress: SC11 MANY pass=%d mode=%s entries=200 eof=1 calls=201 "
                                                     "cost=%d accounting=PASS" % (i,mode,PASS_COST))
    for i,(cookie,cost,result,name) in enumerate(PROBES):
        line="storage-stress: SC11 PROBE %d cookie=%d result=%s cost=%d consumed=%d" % (i,cookie,result,cost,min(cost,LIMIT))
        if name:
            line+=" name=%s next=524288" % name
        yield dict(case="probe %d" % i,kind="probe",result=result),line+" accounting=PASS"
    yield dict(case="enumeration",kind="total"),"storage-stress: SC11 ENUMERATION calls=814 entries=803 eof=5 eagain=3 einval=3 accounting=PASS"
    for i,(kind,result) in enumerate(CHECKS):
        yield dict(case="check %d" % i,kind="check"),"storage-stress: SC11 CHECK %d kind=%s result=%s dest=UNCHANGED accounting=PASS" % (i,kind,result)
    o=orphan(transport)
    yield dict(case="check 3",kind="check",orphan=o),("storage-stress: SC11 CHECK 3 kind=kill-held-probe orphan=%d kill=PASS "
                                                       "publication=NONE accounting=PASS" % o)

CASES={"SC10":sc10_cases,"SC11":sc11_records}

def not_run(groups):
    return "storage-stress: "+" ".join(g for g in ORDER if g not in groups)+" NOT RUN"

def trusted_lines(serial):
    return [x.removesuffix("\r") for x in serial.split("\n") if not x.startswith("user[")]

def validate_serial(serial, groups, transport, exit_status):
    require(exit_status==33,"wrong whole-boot exit")
    require(transport in TRANSPORTS,"unknown transport")
    groups=selection(",".join(groups))
    lines=trusted_lines(serial)
    require(not any(re.search(r"FAILED|FAIL:|VOS-TEST: FAIL|panic:|check failed",x) for x in lines),
            "trusted guest failure")
    expected_transport="storage-stress: TRANSPORT "+transport+" PASS"
    require(lines.count(expected_transport)==1,"missing/duplicate/wrong transport")
    records=[x for x in lines if x.startswith("storage-stress:")]
    require(records and records[0]==expected_transport,"transport/selection record order")
    index=1
    summary=dict(groups=list(groups),transport=transport,SC10_cases=0,SC10_orphans=0,SC11_passes=0,SC11_calls=0,
                 SC11_probes=0,SC11_checks=0,SC11_orphans=0)
    for group in groups:
        require(index<len(records) and records[index]==BEGIN[group],"missing/duplicate/out-of-order group begin")
        index+=1
        for facts,want in CASES[group](transport):
            require(index<len(records) and records[index]==want,
                    "incomplete/out-of-order/inexact "+group+" case "+str(facts["case"]))
            index+=1
            if group=="SC10":
                summary["SC10_cases"]+=1;summary["SC10_orphans"]+=facts["orphan"]
            elif facts["kind"]=="many":
                summary["SC11_passes"]+=1;summary["SC11_calls"]+=201
            elif facts["kind"]=="probe":
                summary["SC11_probes"]+=1;summary["SC11_calls"]+=1
            elif facts["kind"]=="check":
                summary["SC11_checks"]+=1;summary["SC11_orphans"]+=facts.get("orphan",0)
        require(index<len(records) and records[index]==END[group],"missing/duplicate/incomplete group result")
        index+=1
    require(records[index:index+1]==[not_run(groups)],"missing/inexact explicit nonexecution record");index+=1
    require(index<len(records) and re.fullmatch(r"storage-stress: CHECKS [1-9][0-9]* PASS",records[index]),
            "missing exact trusted assertion count")
    summary["checks"]=int(records[index].split()[2]);index+=1
    require(records[index:]==["storage-stress: SELECTED PASS"],"unexpected execution/duplicate/out-of-order tail")
    require(lines.count("VOS-TEST: PASS")==1,"missing/duplicate whole-boot verdict")
    require(lines.index("VOS-TEST: PASS")>lines.index("storage-stress: SELECTED PASS"),"premature whole-boot verdict")
    return summary

def configuration(groups, firmware):
    groups=selection(",".join(groups))
    require(firmware in ("BIOS","UEFI"),"unknown firmware")
    modules=[("hello.elf","hello"),("stress.img","storage-stress.img"),("gated.elf","stress-gated")]
    cfg="set timeout=0\nset default=0\nmenuentry 'VOS N3.6 Gate D' {\n"
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
    transport=transport_of(firmware)
    result=dict(passed=False,firmware=firmware,machine="q35" if firmware=="UEFI" else "pc",transport=transport,
                cpu=cpu,memory="128M",groups=list(groups),argv=argv,group_deadline=240,boot_allowance=60)
    seconds=60+240*len(groups)
    s.watch=dict(started=time.monotonic(),serial=out/"serial.log",groups=groups,began={})
    try:
        s.run("run-"+label,["timeout","--kill-after=2",str(seconds),*argv],extra=8*MIB,timeout=seconds+5,expected=33)
        summary=validate_serial((out/"serial.log").read_text(),groups,transport,33)
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
    require(s.family=="n36-stage3-gated-*","Gate D runner requires a Gate D evidence root")
    if args.prepare_media:build_media(s,groups,args.firmware,json.loads((s.root/"build-env.json").read_text()))
    else:print(json.dumps(run_profile(s,groups,args.firmware,args.cpu),indent=2))
if __name__=="__main__":main()
