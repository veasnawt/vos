#!/usr/bin/env python3
"""Copy the Increment 1 evidence that belongs in the repository (logs, JSON, small sources, three screenshots) into
docs/results/usb-inc1-hosted-safety-and-survey/. Binaries, rlibs, ISOs, worker caches and the raw oracle traces stay in build/
(hashes are recorded in manifest.json). Nothing in build/ is modified or removed. Run from the repository root."""
import hashlib, json, shutil, subprocess
from pathlib import Path
ROOT = Path('/home/veasnawt/src/vos-usb-inc1')
B = ROOT / 'build/inc1-1'
E = ROOT / 'docs/results/usb-inc1-hosted-safety-and-survey'
def cp(src, dst):
    dst = E / dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
def gate(name, label, extra=()):
    g = B / name
    for n in ('result.json', 'tests.txt', 'oracle.txt', 'export.txt', 'model-build.txt', 'tests-build.txt', 'crate-build.txt', *extra):
        if (g / n).exists():
            cp(g / n, f'{label}/{n}')
    for f in sorted(g.glob('cf-*.txt')) + sorted(g.glob('cf-*.rs')):
        cp(f, f'{label}/{f.name}')
    if (B / f'final/{name}-run.txt').exists():
        cp(B / f'final/{name}-run.txt', f'{label}/run.txt')
# hosted gates: the evidence of record and every failed or superseded run
gate('gate41-1', 'gates/c41-machine-passed')
gate('gate42-1', 'history/c42a-session-gate-1-failed-3-survivors')
gate('gate42-2', 'gates/c42a-session-passed')
gate('survey-gate-1', 'history/survey-gate-1-failed-4-survivors')
gate('survey-gate-2', 'history/survey-gate-2-superseded-by-panic-free-rewrite')
gate('survey-gate-3', 'gates/survey-passed')
# native QEMU matrices: the final one in full, earlier ones as records
n4 = B / 'native-4'
cp(n4 / 'result.json', 'native/final/result.json')
cp(B / 'final/native-4-run.txt', 'native/final/run.txt')
for case in sorted(p for p in n4.iterdir() if p.is_dir()):
    for n in ('serial.txt', 'command.json'):
        cp(case / n, f'native/final/{case.name}/{n}')
for case in ('bios-survey-one-xhci', 'bios-survey-two-xhci-bridge-ehci', 'uefi-survey-one-xhci'):
    cp(n4 / case / 'screen.png', f'native/final/{case}/screen.png')
for k, label in (('native-1', 'native-1-first-matrix-passed-but-the-log-wrapped'), ('native-2', 'native-2-failed-test-script-defect'), ('native-3', 'native-3-superseded-by-the-panic-free-rewrite')):
    cp(B / k / 'result.json', f'history/{label}/result.json')
    cp(B / 'final' / f'{k}-run.txt', f'history/{label}/run.txt')
cp(B / 'native-1/bios-survey-one-xhci/screen.png', 'history/native-1-first-matrix-passed-but-the-log-wrapped/bios-screen-circular-log-wrap.png')
cp(B / 'native-2/bios-survey-two-xhci-bridge-ehci/serial.txt', 'history/native-2-failed-test-script-defect/failed-case-serial.txt')
# regression by hand
for n in ('q.cfg', 'c.cfg', 'mk-q.txt', 'mk-c.txt', 'serial-q.txt', 'serial-c.txt'):
    cp(B / 'reg' / n, f'regression/{n}')
# measurements
cp(B / 'measure-7/measurements.json', 'measurements/session-frames-and-code.json')
cp(B / 'final/meas/kernel-footprint.json', 'measurements/kernel-footprint.json')
for k in ('measure-1', 'measure-2'):
    pass
# storage and scripts
for n in ('storage-forecast.md', 'storage-log.txt', 'guard.sh', 'storage-investigation-1.md', 'storage-estimation-rules.md'):
    cp(B / 'final' / n, f'storage/{n}')
for n in ('assemble-evidence.py', 'check-anchors.py', 'leak-scan.py', 'make-manifest.py'):
    if (B / 'final' / n).exists():
        cp(B / 'final' / n, f'scripts/{n}')
for n in ('bios-iso-build.txt', 'bios-iso-build-2.txt', 'uefi-iso-build.txt', 'uefi-iso-build-2.txt', 'rebuild-isos.txt', 'rebuild-isos-2.txt', 'kernel-rebuild-3.txt'):
    if (B / 'final' / n).exists():
        cp(B / 'final' / n, f'build-logs/{n}')
print(sum(f.stat().st_size for f in E.rglob('*') if f.is_file()), 'bytes')
