#!/usr/bin/env python3
"""Copy the C4.2a evidence that belongs in the repository (logs, JSON, small sources) into
docs/results/usb2c42a-hosted-backend-contract/. Binaries, rlibs, worker caches and the raw oracle trace stay in build/
(hashes are recorded). Nothing in build/ is modified or removed. Run from the repository root."""
import shutil
from pathlib import Path
ROOT = Path('/home/veasnawt/src/vos-usb2c42a')
B = ROOT / 'build/usb2c42a-1'
E = ROOT / 'docs/results/usb2c42a-hosted-backend-contract'
def cp(src, dst):
    dst = E / dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
def gate(g, name):
  for n in ('result.json', 'tests.txt', 'oracle.txt', 'export.txt', 'model-build.txt', 'tests-build.txt', 'cf-unsafe-rejected.txt'):
    cp(g / n, f'{name}/{n}')
  for f in sorted(g.glob('cf-*.txt')) + sorted(g.glob('cf-*.rs')):
    cp(f, f'{name}/{f.name}')
  cp(B / f'final/{name}-run.txt', f'{name}/run.txt')
gate(B / 'gates-3', 'gates-3')
gate(B / 'gates-4', 'gates-4')
for run, label in (('gates-1', 'gates-1-failed'), ('gates-2', 'gates-2-failed')):
    cp(B / run / 'result.json', f'history/{label}/result.json')
    cp(B / f'final/{run}-run.txt', f'history/{label}/run.txt')
cp(B / 'final/oracle-first-run.txt', 'history/oracle-first-run.txt')
cp(B / 'measure-1/measurements.json', 'history/measure-1-superseded/measurements.json')
cp(B / 'c41-regression/result.json', 'c41-regression/result.json')
cp(B / 'final/c41-regression-run.txt', 'c41-regression/run.txt')
cp(B / 'measure-2/measurements.json', 'measurements/measurements.json')
cp(B / 'final/kernel-iso-build.txt', 'isolation/kernel-iso-build.txt')
cp(B / 'final/identity.json', 'isolation/identity.json')
for n in ('storage-forecast.md', 'storage-log.txt', 'guard.sh'):
    cp(B / 'final' / n, f'storage/{n}')
for n in ('leak-scan.py', 'assemble-evidence.py', 'build-tests.sh'):
    cp(B / 'final' / n, f'scripts/{n}')
print(sum(f.stat().st_size for f in E.rglob('*') if f.is_file()), 'bytes')
