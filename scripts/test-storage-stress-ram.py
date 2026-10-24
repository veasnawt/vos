#!/usr/bin/env python3
"""N3 final campaign C20 only: the accepted Gate B/C/D stress images at larger guest RAM; strict, no retries.

Each run boots one gate's admitted media and is judged by that gate's own runner (selection, transport, serial
contract and group watchdog), loaded unchanged. Only the guest memory differs from the accepted 128 MiB runs:
BIOS/pc 2 GiB on the default CPU, and UEFI/q35 1 GiB and sparse 8 GiB on the default and max CPU (the N3.5
larger-RAM precedent). A sparse guest's RAM is a file-backed sparse file in its run directory, as in
scripts/exit-test.sh; its allocation is bounded while the guest runs and the file is removed afterwards.
The kernel's single pmm record must report 0 bytes of RAM above 1 GiB at 1 GiB and a positive count above it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time
from importlib.util import module_from_spec, spec_from_file_location
from storage_stress_b_host import FINAL, MIB, allocated, require
from uefi_catalog import validate_iso

HERE=Path(__file__).resolve().parent
GATES={"b":("test-storage-stress-b.py",("SC01","SC02")),"c":("test-storage-stress-c.py",("SC03","SC04","SC05")),
       "d":("test-storage-stress-d.py",("SC10","SC11"))}
PROFILES=(("BIOS","default","2G"),("UEFI","default","1G"),("UEFI","max","1G"),("UEFI","default","8G"),("UEFI","max","8G"))
SPARSE=("8G",)
RAM_ALLOCATION_LIMIT=384*MIB                # bound on one sparse guest's allocated backing while it runs
SPARSE_FILE_LIMIT=8*1024*MIB+64*MIB         # RLIMIT_FSIZE for a sparse guest: the backing's apparent size, never its allocation
PMM=re.compile(r"pmm: \d+ frames tracked below 1 GiB, \d+ usable \(\d+ KiB\), (\d+) bytes of RAM above 1 GiB ignored")

def gate_runner(gate):
    require(gate in GATES,"unknown gate")
    spec=spec_from_file_location("gate_"+gate+"_runner",HERE/GATES[gate][0])
    module=module_from_spec(spec);spec.loader.exec_module(module)
    return module

def plan():
    """The 15 C20 runs, in execution order: each gate on every larger-RAM profile."""
    return [(gate,firmware,cpu,memory) for gate in GATES for firmware,cpu,memory in PROFILES]

def admitted_profile(firmware, cpu, memory):
    require((firmware,cpu,memory) in PROFILES,"profile outside the C20 larger-RAM matrix")

def transport_of(firmware):
    require(firmware in ("BIOS","UEFI"),"unknown firmware")
    return "POLL" if firmware=="UEFI" else "IRQ"

def memory_args(memory, out):
    args=["-m",memory]
    if memory in SPARSE:
        args+=["-object","memory-backend-file,id=ram,size="+memory+",mem-path="+str(out/"guest.ram")+",share=on",
               "-machine","memory-backend=ram"]
    return args

def qemu_argv(r, iso, out, firmware, cpu, memory):
    """The accepted runners' QEMU command, with only the guest memory arguments changed."""
    argv=["qemu-system-x86_64","-machine","q35" if firmware=="UEFI" else "pc",*memory_args(memory,out)]
    if cpu=="max":argv+=["-cpu","max"]
    if firmware=="UEFI":
        argv+=["-drive","if=pflash,format=raw,readonly=on,file="+str(r/"inputs/OVMF_CODE_4M.fd"),
               "-drive","if=pflash,format=raw,file="+str(out/"vars.fd")]
    argv+=["-cdrom",str(iso),"-boot","d","-drive",
           "file="+str(r/"inputs/stress.img")+",if=none,id=vd0,format=raw,readonly=on",
           "-device","virtio-blk-pci,drive=vd0,disable-legacy=on","-display","none","-no-reboot","-nic","none",
           "-serial","file:"+str(out/"serial.log"),"-device","isa-debug-exit,iobase=0xf4,iosize=0x04",
           "-d","cpu_reset,guest_errors","-D",str(out/"qemu.log")]
    return argv

def ignored_ram(serial, trusted_lines):
    found=[int(m.group(1)) for m in (PMM.fullmatch(x) for x in trusted_lines(serial)) if m]
    require(len(found)==1,"missing/duplicate pmm memory record")
    return found[0]

def check_memory(serial, trusted_lines, memory):
    ignored=ignored_ram(serial,trusted_lines)
    require(ignored==0 if memory=="1G" else ignored>0,"guest memory not as configured: %d bytes above 1 GiB ignored" % ignored)
    return ignored

def judge(runner, serial, groups, transport, memory):
    """The gate's own unchanged serial contract, then the memory record."""
    summary=runner.validate_serial(serial,groups,transport,33)
    return summary,check_memory(serial,runner.trusted_lines,memory)

def require_final(s):
    require(s.family==FINAL,"the larger-RAM wrapper requires an N3 final-campaign evidence root")

def ram_guard(runner):
    """The gate's RunGuard (group watchdog, boot allowance, cumulative storage), plus the sparse-RAM bound."""
    class RamGuard(runner.RunGuard):
        def guard(self,extra=0):
            row=super().guard(extra)
            ram=getattr(self,"ram",None)
            if ram is not None and os.path.lexists(ram):
                require(allocated(ram)<=RAM_ALLOCATION_LIMIT,"sparse guest RAM allocation bound")
            return row
    return RamGuard

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run_profile(s, gate, firmware, cpu, memory):
    runner=s.runner;groups=GATES[gate][1]
    admitted_profile(firmware,cpu,memory)
    require(runner.selection(",".join(groups))==groups,"gate selection")
    sparse=memory in SPARSE
    r=s.root;key=firmware.lower()+"-"+"-".join(groups);label=firmware.lower()+"-"+cpu+"-"+memory+"-"+"-".join(groups)
    s.guard(12*MIB+(RAM_ALLOCATION_LIMIT if sparse else 0))
    admitted=json.loads((r/"preboot.json").read_text())
    require(admitted["production_isolation"] and admitted["frozen_hashes"]==139,"production admission missing")
    for rel,want in admitted["sha256"].items():
        p=r/rel;require(p.resolve().is_relative_to(r) and digest(p)==want,"preboot provenance "+rel)
    iso=r/"media"/key/"vos.iso"
    require(str(iso.relative_to(r)) in admitted["sha256"],"selected media not admitted")
    env=json.loads((r/"build-env.json").read_text())
    if firmware=="UEFI":validate_iso(iso,r/("preboot-"+label),s,env)
    out=r/("run-"+label);require(not out.exists(),"refusing attempt overwrite");out.mkdir()
    if firmware=="UEFI":s.copy(r/"inputs/OVMF_VARS_4M.fd",out/"vars.fd")
    argv=qemu_argv(r,iso,out,firmware,cpu,memory)
    window=r/"execution-window.json"
    if not window.exists():
        with window.open("x") as f:f.write(json.dumps(dict(started_monotonic=time.monotonic(),utc=time.time()))+"\n")
    transport=transport_of(firmware)
    result=dict(passed=False,gate=gate,firmware=firmware,machine="q35" if firmware=="UEFI" else "pc",transport=transport,
                cpu=cpu,memory=memory,sparse=sparse,groups=list(groups),argv=argv,group_deadline=240,boot_allowance=60)
    seconds=60+240*len(groups)
    ram=out/"guest.ram"
    s.ram=ram if sparse else None
    s.watch=dict(started=time.monotonic(),serial=out/"serial.log",groups=groups,transport=transport,began={})
    try:
        s.run("run-"+label,["timeout","--kill-after=2",str(seconds),*argv],
              extra=8*MIB+(RAM_ALLOCATION_LIMIT if sparse else 0),timeout=seconds+5,expected=33,
              **({"file_limit":SPARSE_FILE_LIMIT} if sparse else {}))
        if sparse:require(os.path.lexists(ram) and allocated(ram)<=RAM_ALLOCATION_LIMIT,"sparse guest RAM allocation bound")
        summary,ignored=judge(runner,(out/"serial.log").read_text(),groups,transport,memory)
        require("Triple fault" not in (out/"qemu.log").read_text(),"triple fault")
        for rel,want in admitted["sha256"].items():require(digest(r/rel)==want,"input changed during run "+rel)
        result.update(passed=True,exit_status=33,summary=summary,ignored_bytes_above_1gib=ignored,
                      seconds=time.monotonic()-s.watch["started"],serial_sha256=digest(out/"serial.log"))
    finally:
        s.watch=None;s.ram=None
        if sparse:
            if os.path.lexists(ram):
                st=ram.lstat();result.update(guest_ram_apparent=st.st_size,guest_ram_allocated=st.st_blocks*512)
                ram.unlink()
            result["guest_ram_removed"]=not os.path.lexists(ram)
        with (out/"result.json").open("x") as f:f.write(json.dumps(result,indent=2)+"\n")
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("evidence",type=Path)
    p.add_argument("--gate",choices=sorted(GATES),required=True)
    p.add_argument("--firmware",choices=["BIOS","UEFI"],required=True)
    p.add_argument("--cpu",choices=["default","max"],required=True)
    p.add_argument("--memory",choices=sorted({m for _,_,m in PROFILES}),required=True)
    args=p.parse_args();admitted_profile(args.firmware,args.cpu,args.memory)
    runner=gate_runner(args.gate);s=ram_guard(runner)(args.evidence);require_final(s);s.runner=runner
    print(json.dumps(run_profile(s,args.gate,args.firmware,args.cpu,args.memory),indent=2))
if __name__=="__main__":main()
