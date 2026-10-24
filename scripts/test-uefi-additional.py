#!/usr/bin/env python3
"""N3 final campaign D1c: the 19 additional UEFI configurations of N3.5 Stage 5, reconstructed; no retries.

The original command file (remaining-verification.sh) was not preserved. These runs are reconstructed from the
Stage 5 report's table (docs/results/n3.5-stage5.md, "Additional fresh UEFI configurations: all 19 runs"): the same
names, images, RAM sizes and CPUs, through the unchanged scripts/exit-test.sh with OVMF (q35, fresh variables per
run) and sparse file-backed RAM at 8 GiB, exactly as make test-uefi runs its own 8 GiB guest. Markers follow the
report: every file run 845/0, every reader run 97/0, every run io-scope 18/0, 8 GiB runs also the ignored-RAM
record; the report does not list the base run's markers, so it uses the pmm-kernel marker of make test-uefi's
8 GiB base run. After each run the serial log must hold exactly one pmm record, reporting 0 bytes above 1 GiB at or
below 1 GiB and a positive count at 8 GiB: stricter than the report's substring marker, which every boot prints.

This module only describes and checks the runs; the guarded campaign driver executes them, one at a time, from a
tree in which make test-uefi has built build/x86_64-test{,-file,-ext2}/vos-uefi.iso and build/ext2.
usage: test-uefi-additional.py --list
"""
import json
from pathlib import Path
import re
import sys

ISO={"base":"build/x86_64-test/vos-uefi.iso","file":"build/x86_64-test-file/vos-uefi.iso",
     "ext2":"build/x86_64-test-ext2/vos-uefi.iso"}
EXT2_DISK=["-drive","file=build/ext2/ext2-1k.img,if=none,id=vd0,format=raw,readonly=on",
           "-device","virtio-blk-pci,drive=vd0,disable-legacy=on"]
SIZES={"default":("128M","1G","8G"),"max":("128M","1G","8G","192M","256M","512M")}
RUNS=[("i5-base-1G","base","1G","default")]+[("i5-%s-%s-%s" % (image,cpu,mem),image,mem,cpu)
      for image in ("file","ext2") for cpu in ("default","max") for mem in SIZES[cpu]]
SCOPE="selftest: io-scope 18 cases, 0 failed"
MARKERS={"base":["selftest: pmm-kernel ",SCOPE,"VOS-TEST: PASS"],
         "file":["selftest: file 845 cases, 0 failed",SCOPE,"VOS-TEST: PASS"],
         "ext2":["selftest: ext2 97 cases, 0 failed, ",SCOPE,"VOS-TEST: PASS"]}
ABOVE_1G="bytes of RAM above 1 GiB ignored"
SPARSE=("8G",)
PMM=re.compile(r"pmm: \d+ frames tracked below 1 GiB, \d+ usable \(\d+ KiB\), (\d+) bytes of RAM above 1 GiB ignored")

def require(ok, why):
    if not ok:
        raise RuntimeError(why)

def command(name, image, mem, cpu):
    """(environment, argv, serial log) for one run, relative to the built tree."""
    markers=MARKERS[image]+([ABOVE_1G] if mem in SPARSE else [])
    qemu=(EXT2_DISK if image!="base" else [])+(["-cpu","max"] if cpu=="max" else [])
    env={"OVMF":"1","MEM":mem,"TIMEOUT":"240"}
    if mem in SPARSE:env["SPARSE_MEM"]="1"
    argv=["scripts/exit-test.sh",name,ISO[image],"33",*markers,"--",*qemu]
    return env,argv,str(Path(ISO[image]).parent/(name+".serial.log"))

def commands():
    require(len(RUNS)==19 and len({r[0] for r in RUNS})==19,"exactly 19 distinct runs")
    return [dict(name=n,image=i,memory=m,cpu=c,env=e,argv=a,serial=s) for n,i,m,c in RUNS for e,a,s in [command(n,i,m,c)]]

def trusted_lines(serial):
    return [x.removesuffix("\r") for x in serial.split("\n") if not x.startswith("user[")]

def check_serial(serial, mem):
    """Exactly one pmm record: 0 bytes above 1 GiB at or below 1 GiB, a positive count at 8 GiB."""
    found=[int(m.group(1)) for m in (PMM.fullmatch(x) for x in trusted_lines(serial)) if m]
    require(len(found)==1,"missing/duplicate pmm memory record")
    require(found[0]>0 if mem in SPARSE else found[0]==0,"guest memory not as configured: %d bytes above 1 GiB ignored" % found[0])
    return found[0]

if __name__=="__main__":
    if sys.argv[1:]!=["--list"]:
        sys.exit(__doc__.strip().splitlines()[-1])
    print(json.dumps(commands(),indent=1))
