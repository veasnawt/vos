#!/usr/bin/env python3
"""Summarise an exported C4.1 trace into a refusal / rule / operation matrix (read-only)."""
import collections, json, re, sys
reasons = collections.defaultdict(lambda: collections.Counter())
rules = collections.defaultdict(lambda: collections.Counter())
ops = collections.Counter()
phase_of = {}
for line in open(sys.argv[1]):
    if not line.startswith('S '):
        continue
    op, rest = line[2:].rstrip('\n').split(' => ')
    name = op.split()[0]
    out = rest.split(' @ ')[0].split()
    ops[name] += 1
    if out[0] == 'err':
        reasons[out[1]][name] += 1
    else:
        rules[out[2]][name] += 1
res = {'operations_seen': dict(ops),
       'reasons': {k: {'count': sum(v.values()), 'operations': dict(v.most_common(4))} for k, v in sorted(reasons.items())},
       'rules': {k: {'count': sum(v.values()), 'operations': dict(v.most_common(3))} for k, v in sorted(rules.items())}}
json.dump(res, open(sys.argv[2], 'w'), indent=1)
md = ['# Refusal and justification matrix (measured on the exported oracle trace)', '',
      f'Operations exercised: {len(ops)} distinct, {sum(ops.values())} steps. Named refusals seen in the trace: {len(reasons)}; accepted rules seen: {len(rules)}.',
      'Reasons that occur only as terminal failure causes (not as refusals) appear in the Rust coverage test, not here.', '',
      '## Refusals (reason: count, producing operations)', '']
for k, v in res['reasons'].items():
    md.append(f"- `{k}`: {v['count']} ({', '.join(f'{o} {c}' for o, c in v['operations'].items())})")
md += ['', '## Accepted transitions by justifying rule', '']
for k, v in res['rules'].items():
    md.append(f"- `{k}`: {v['count']} ({', '.join(f'{o} {c}' for o, c in v['operations'].items())})")
open(sys.argv[3], 'w').write('\n'.join(md) + '\n')
print(len(ops), len(reasons), len(rules))
