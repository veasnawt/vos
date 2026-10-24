#!/usr/bin/env python3
"""Gate C only: explicit selection, admitted media, strict SC03/SC04/SC05 serial contracts, no retries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
from storage_stress_b_host import Guard, MIB, require
from uefi_catalog import validate_iso
ORDER=("SC01","SC02","SC03","SC04","SC05","SC10","SC11")
IMPLEMENTED=("SC03","SC04","SC05")
TRANSPORTS=("IRQ","POLL")
OPS=("open","read","stat","enum")
MIXES=("AA","AO","OO")
FAULTS=("line-fallback","prefix-eio","eintr")
NOT_APPLICABLE=" NOT APPLICABLE (polling transport: a killed polling waiter leaves no orphan)"

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
 "SC03":"storage-stress: SC03 BEGIN seed=0x4E360003 cases=32",
 "SC04":"storage-stress: SC04 BEGIN seed=0x4E360004 cases=12",
 "SC05":"storage-stress: SC05 BEGIN seed=0x4E360005 cases=12"}

def end(group, transport):
    require(transport in TRANSPORTS,"unknown transport")
    if group=="SC03":return "storage-stress: SC03 PASS cases=32 kills=32 publication=NONE accounting=PASS"
    if group=="SC04":
        applicable=12 if transport=="IRQ" else 4
        return ("storage-stress: SC04 PASS applicable="+str(applicable)+
                " not_applicable="+str(12-applicable)+" accounting=PASS")
    return "storage-stress: SC05 PASS cases=12 accounting=PASS"

def sc03_cases(transport):
    """32 cases: op x wait site x order x reuse. Only an interrupt-mode device kill leaves an orphan."""
    for n in range(32):
        op,site,order,reuse=OPS[n//8],("device","reader")[n//4%2],("kill-first","complete-first")[n//2%2],("same","other")[n%2]
        orphan=int(site=="device" and order=="kill-first" and transport=="IRQ")
        yield dict(case=n,orphan=orphan),("storage-stress: SC03 CASE "+str(n)+" op="+op+" site="+site+" order="+order+
                                           " reuse="+reuse+" orphan="+str(orphan)+" kill=PASS publication=NONE accounting=PASS")

def sc04_cases(transport):
    """12 cases: AA/AO/OO x order x round. Under polling only AA applies; the rest are NOT APPLICABLE, never passes."""
    for n in range(12):
        mix,order,rnd=n//4,n//2%2,n%2
        head="storage-stress: SC04 CASE "+str(n)+" mix="+MIXES[mix]+" order="+str(order)+" round="+str(rnd)
        if mix and transport!="IRQ":
            yield dict(case=n,applicable=False,orphans=0),head+NOT_APPLICABLE
        else:
            yield dict(case=n,applicable=True,orphans=mix),head+" orphans="+str(mix)+" retire=EXACT accounting=PASS"

def sc05_cases():
    """12 cases: round x fault x role, in that loop order."""
    for n in range(12):
        rnd,fault,role=n//6,n//2%3,n%2
        yield dict(case=n),("storage-stress: SC05 CASE "+str(n)+" fault="+FAULTS[fault]+" role="+("owner","waiter")[role]+
                            " round="+str(rnd)+(" dest=EXACT" if fault==0 else " dest=UNCHANGED")+" retry=PASS accounting=PASS")

CASES={"SC03":sc03_cases,"SC04":sc04_cases,"SC05":lambda transport:sc05_cases()}

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
    summary=dict(groups=list(groups),transport=transport,SC03_cases=0,SC03_kills=0,SC03_orphans=0,
                 SC04_applicable=0,SC04_not_applicable=0,SC04_orphans=0,SC05_cases=0)
    for group in groups:
        require(index<len(records) and records[index]==BEGIN[group],"missing/duplicate/out-of-order group begin")
        index+=1
        for facts,want in CASES[group](transport):
            require(index<len(records) and records[index]==want,
                    "incomplete/out-of-order/inexact "+group+" case "+str(facts["case"]))
            index+=1
            if group=="SC03":
                summary["SC03_cases"]+=1;summary["SC03_kills"]+=1;summary["SC03_orphans"]+=facts["orphan"]
            elif group=="SC04":
                summary["SC04_applicable" if facts["applicable"] else "SC04_not_applicable"]+=1
                summary["SC04_orphans"]+=facts["orphans"]
            else:
                summary["SC05_cases"]+=1
        require(index<len(records) and records[index]==end(group,transport),"missing/duplicate/incomplete group result")
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
    modules=[("hello.elf","hello"),("stress.img","storage-stress.img"),("gatec.elf","stress-gatec")]
    cfg="set timeout=0\nset default=0\nmenuentry 'VOS N3.6 Gate C' {\n"
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
                if group in watch["began"] and end(group,watch["transport"]) not in lines:
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
    s.watch=dict(started=time.monotonic(),serial=out/"serial.log",groups=groups,transport=transport,began={})
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
    require(s.gatec,"Gate C runner requires a Gate C evidence root")
    if args.prepare_media:build_media(s,groups,args.firmware,json.loads((s.root/"build-env.json").read_text()))
    else:print(json.dumps(run_profile(s,groups,args.firmware,args.cpu),indent=2))
if __name__=="__main__":main()
