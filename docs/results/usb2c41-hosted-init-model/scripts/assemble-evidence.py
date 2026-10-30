#!/usr/bin/env python3
"""Copy the C4.1 evidence that belongs in the repository (logs, JSON, small sources) into
docs/results/usb2c41-hosted-init-model/. Binaries, rlibs and the raw oracle trace stay in build/ (hashes are
recorded). Nothing in build/ is modified or removed. Run from the repository root."""
import json, shutil
from pathlib import Path
ROOT = Path('/home/veasnawt/src/vos-usb2c41')
B = ROOT / 'build/usb2c41-1'
E = ROOT / 'docs/results/usb2c41-hosted-init-model'
E.mkdir(parents=True, exist_ok=True)
def cp(src, dst):
    dst = E / dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
g3 = B / 'gates-3'
for n in ('result.json', 'tests.txt', 'oracle.txt', 'export.txt', 'model-build.txt', 'tests-build.txt',
          'cf-old-broad-program-action-is-gone.txt', 'cf-old-broad-program-action-is-gone.rs',
          'cf-registers-have-no-raw-write.txt', 'cf-registers-have-no-raw-write.rs',
          'cf-state-cannot-be-forged.txt', 'cf-state-cannot-be-forged.rs', 'cf-unsafe-rejected.txt'):
    cp(g3 / n, f'gates-3/{n}')
cp(B / 'final/gates-3-run.txt', 'gates-3/run.txt')
for run, label in (('gates-1', 'gates-1-failed'), ('gates-2', 'gates-2-superseded')):
    cp(B / run / 'result.json', f'history/{label}/result.json')
    cp(B / f'final/{run}-run.txt', f'history/{label}/run.txt')
cp(B / 'measure-2/measurements.json', 'history/measure-2-superseded/measurements.json')
cp(B / 'measure-3/measurements.json', 'measurements/measurements.json')
cp(B / 'final/kernel-identity-build.txt', 'isolation/kernel-identity-build.txt')
cp(B / 'final/identity.json', 'isolation/identity.json')
for n in ('storage-forecast.md', 'storage-log.txt', 'guard.sh'):
    cp(B / 'final' / n, f'storage/{n}')
cp(B / 'final/refusal-and-rule-matrix.md', 'matrix/refusal-and-rule-matrix.md')
cp(B / 'final/trace-summary.json', 'matrix/trace-summary.json')
for n in ('summarize-trace.py', 'leak-scan.py', 'assemble-evidence.py', 'build-tests.sh'):
    cp(B / 'final' / n, f'scripts/{n}')
print(sum(f.stat().st_size for f in E.rglob('*') if f.is_file()), 'bytes')
