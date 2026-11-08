#!/usr/bin/env python3
"""Prove the Freestanding CI split is a complete, unweakened partition of the original single job, and that the required
aggregator only passes when every group succeeded.

Compares the original workflow (--baseline, e.g. `git show <main>:.github/workflows/build-smoke.yml`) with the split one:
- every original verification step appears in exactly one group, byte-for-byte identical as a parsed step (name, run, shell,
  env, with, if, timeout-minutes, uses) and in its original relative order;
- every group starts with the identical setup steps (checkout, prerequisites, toolchain, tool versions, reproducible build),
  keeps the original job settings (runner, 45-minute limit, no extra permissions, no continue-on-error) and has its own
  failure-only text-log artifact, identical to the original except for a distinct name;
- the legacy i386 check and the user ELF profiles stay in the core group, before the steps that need them;
- exactly one job is named "Freestanding builds and user ELF static tests": the aggregator, which needs exactly the groups,
  always runs, and whose decision script passes only when every group's result is "success" (exercised here with success,
  failure, skipped, cancelled, missing-group and extra-group inputs);
- every other job and the workflow's triggers, permissions and concurrency are unchanged.
Writes a JSON report with --out. Requires PyYAML. Not a CI step: it is the publication evidence for the split."""
import argparse, json, os, subprocess, sys
from pathlib import Path
import yaml
REQUIRED = 'Freestanding builds and user ELF static tests'
GROUPS = ('freestanding-core', 'freestanding-graphics', 'freestanding-usb')
SETUP = ('Checkout repository', 'Install build prerequisites', 'Prepare the existing bare-metal Rust toolchain', 'Record tool versions', 'Build x86_64 BIOS and UEFI ISOs')
ARTIFACT = 'Keep the text logs of a failed run'
CORE_ONLY = ('Verify legacy i386 Multiboot kernel', 'Validate separately built user ELF profiles')
def need(x, s):
    if not x: raise SystemExit('PARTITION FAIL: ' + s)
def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, required=True); ap.add_argument('--workflow', type=Path, required=True); ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    old = yaml.safe_load(a.baseline.read_text()); new = yaml.safe_load(a.workflow.read_text())
    # PyYAML reads the "on" key as True
    for k in (True, 'permissions', 'concurrency', 'name'): need(old.get(k) == new.get(k), 'top-level %r unchanged' % k)
    oj, nj = old['jobs'], new['jobs']
    need(set(oj) - {'build-smoke'} <= set(nj) and all(oj[k] == nj[k] for k in oj if k != 'build-smoke'), 'other jobs unchanged')
    need(set(nj) == set(oj) | set(GROUPS), 'jobs are exactly the original ones plus the three groups')
    named = [k for k, v in nj.items() if v.get('name') == REQUIRED]; need(named == ['build-smoke'], 'exactly one job carries the required name')
    base = oj['build-smoke']; steps = base['steps']; names = [s['name'] for s in steps]
    need(len(set(names)) == len(names), 'original step names unique')
    verify = [s for s in steps if s['name'] not in SETUP and s['name'] != ARTIFACT]
    report = {'original_steps': len(steps), 'verification_steps': len(verify), 'groups': {}}
    seen = {}
    for g in GROUPS:
        job = nj[g]
        for k in ('runs-on', 'timeout-minutes'): need(job.get(k) == base.get(k), '%s keeps %s' % (g, k))
        need('permissions' not in job and 'continue-on-error' not in job and 'needs' not in job and 'if' not in job, '%s adds no permissions/continue-on-error/dependencies/conditions' % g)
        gs = job['steps']; gnames = [s['name'] for s in gs]
        need(gs[:len(SETUP) + (1 if g == 'freestanding-core' else 0)] == [s for s in steps if s['name'] in SETUP or (g == 'freestanding-core' and s['name'] == CORE_ONLY[0])], '%s starts with the identical setup steps' % g)
        art = gs[-1]; oart = next(s for s in steps if s['name'] == ARTIFACT)
        need(art['name'] == ARTIFACT and art.get('if') == 'failure()' and art['uses'] == oart['uses'], '%s ends with the failure-only artifact' % g)
        need({**art, 'with': {**art['with'], 'name': oart['with']['name']}} == oart and art['with']['name'] == 'build-smoke-logs-' + g.split('-', 1)[1], '%s artifact identical except its distinct name' % g)
        mine = [s for s in gs if s['name'] not in SETUP and s['name'] != ARTIFACT]
        for s in mine:
            need(s in verify, '%s step %r is an unmodified original step' % (g, s['name'])); need(s['name'] not in seen, 'step %r in one group only' % s['name']); seen[s['name']] = g
        need([s['name'] for s in mine] == [s['name'] for s in verify if s in mine], '%s keeps the original order' % g)
        report['groups'][g] = [s['name'] for s in mine]
    need(set(seen) == {s['name'] for s in verify}, 'every original verification step is in a group: missing %s' % sorted({s['name'] for s in verify} - set(seen)))
    need(all(seen[n] == 'freestanding-core' for n in CORE_ONLY), 'legacy i386 and user ELF profiles are core steps')
    core = [s['name'] for s in nj['freestanding-core']['steps']]
    need(core.index('Validate separately built user ELF profiles') < core.index('Verify migrated ELF bounds and hosted compatibility'), 'user ELFs are built before the ELF bounds check')
    arts = [nj[g]['steps'][-1]['with']['name'] for g in GROUPS]; need(len(set(arts)) == 3, 'distinct artifact names')
    agg = nj['build-smoke']
    need(agg.get('needs') == list(GROUPS) and agg.get('if') == '${{ always() }}' and len(agg['steps']) == 1, 'aggregator needs exactly the groups and always runs')
    need('permissions' not in agg and 'continue-on-error' not in agg, 'aggregator adds no permissions or continue-on-error')
    step = agg['steps'][0]; need(step['env'] == {'NEEDS': '${{ toJSON(needs) }}'}, 'aggregator reads only the needs context, through the environment')
    script = step['run']; need(script.startswith("python3 -I -B - <<'PY'\n") and script.rstrip().endswith('PY'), 'aggregator script shape')
    body = script.split("<<'PY'\n", 1)[1].rsplit('PY', 1)[0]
    def decide(needs):
        p = subprocess.run([sys.executable, '-I', '-B', '-'], input=body, text=True, capture_output=True, env={'NEEDS': json.dumps(needs), 'PATH': os.environ.get('PATH', '')})
        return p.returncode
    ok = {g: {'result': 'success', 'outputs': {}} for g in GROUPS}
    cases = {'all-success': (ok, 0)}
    for g in GROUPS:
        for r in ('failure', 'skipped', 'cancelled', None):
            n = json.loads(json.dumps(ok))
            if r is None: n.pop(g)
            else: n[g]['result'] = r
            cases['%s-%s' % (g, r or 'missing')] = (n, 1)
    cases['extra-group'] = ({**ok, 'unexpected': {'result': 'success'}}, 1)
    cases['empty'] = ({}, 1)
    report['aggregator_cases'] = {}
    for k, (n, want) in cases.items():
        rc = decide(n); report['aggregator_cases'][k] = {'expected_success': want == 0, 'exit': rc}
        need((rc == 0) == (want == 0), 'aggregator decision for %s' % k)
    report['status'] = 'PASS'
    if a.out: a.out.write_text(json.dumps(report, indent=2) + '\n')
    print('PARTITION PASS: %d original steps, %d verification steps in %s; %d aggregator cases'
          % (len(steps), len(verify), {g: len(v) for g, v in report['groups'].items()}, len(cases)))
if __name__ == '__main__': main()
