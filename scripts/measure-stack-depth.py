#!/usr/bin/env python3
"""USB.2-C3b.2 static worst-case stack depth below a root function of a linked kernel (read-only analysis).

Frame size per function = pushes + the first `sub $imm,%rsp` of its prologue, from the disassembly; every call
adds its 8-byte return address. Direct calls are followed exactly. Indirect calls (trait objects and closures)
are resolved conservatively: a function containing one may call any function whose name marks a trait-object
implementation in the USB.2 modules. This is an upper-bound estimate from static information, not a measurement
of a running kernel; recursion is reported if the graph has a cycle on the path.
usage: measure-stack-depth.py KERNEL ROOT [ROOT...]
"""
import json
import re
import subprocess
import sys

DYN = re.compile(r'(Source|Effects)>::|\{closure#|call_once|containment::(rehearse|stages|execute)|xhci_contain::execute::session|inspect')


def parse(kernel):
    text = subprocess.check_output(['objdump', '-d', '--no-show-raw-insn', kernel], text=True)
    funcs, cur = {}, None
    for line in text.splitlines():
        m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if m:
            cur = funcs.setdefault(m.group(1), {'calls': set(), 'indirect': False, 'frame': 0, 'n': 0, 'pushes': 0, 'sub': 0})
            continue
        if not cur:
            continue
        m = re.match(r'\s*[0-9a-f]+:\s+(\S+)\s*(.*)', line)
        if not m:
            continue
        op, rest = m.group(1), m.group(2)
        cur['n'] += 1
        if cur['n'] <= 48:
            if op == 'push':
                cur['pushes'] += 1
            elif op == 'sub' and re.match(r'\$0x[0-9a-f]+,%rsp', rest) and not cur['sub']:
                cur['sub'] = int(rest.split(',')[0][1:], 16)
        c = re.match(r'(?:call|jmp)\s+[0-9a-f]+ <([^>+]+)(?:\+0x[0-9a-f]+)?>', op + ' ' + rest)
        if c and op == 'call':
            cur['calls'].add(c.group(1))
        elif op == 'call' and rest.startswith('*'):
            cur['indirect'] = True
        elif op == 'jmp' and c:
            cur['calls'].add(c.group(1))   # tail call: same frame depth class, followed as a call
    for f in funcs.values():
        f['frame'] = 8 * f['pushes'] + f['sub']
    return funcs


def depth(funcs, root):
    dyn = [n for n in funcs if DYN.search(n)]
    memo, stack = {}, []

    def go(name):
        if name in memo:
            return memo[name]
        if name in stack:
            return (0, [name + ' (cycle)'])
        f = funcs.get(name)
        if not f:
            return (0, [name])
        stack.append(name)
        best = (0, [])
        for callee in sorted(f['calls'] | (set(dyn) if f['indirect'] else set())):
            if callee == name:
                continue
            d, path = go(callee)
            if d + 8 > best[0]:
                best = (d + 8, path)
        stack.pop()
        memo[name] = (f['frame'] + best[0], [name] + best[1])
        return memo[name]
    return go(root)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    funcs = parse(argv[1])
    out = {}
    for root in argv[2:]:
        d, path = depth(funcs, root)
        out[root] = {'worst_case_bytes': d, 'path': [{'function': p, 'frame': funcs.get(p, {}).get('frame')} for p in path]}
    print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main(sys.argv)
