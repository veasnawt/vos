import re, sys
from pathlib import Path
root = Path('/home/veasnawt/src/vos-usb-inc1')
script, base = sys.argv[1], sys.argv[2]
default = sys.argv[3] if len(sys.argv) > 3 else 'session.rs'
src = (root / base)
muts = []
def mut(name, why, old, new, file=None):
    file = file or default
    muts.append((name, old, new, file))
buf = ''
for line in (root / script).read_text().splitlines():
    if buf or line.startswith('mut('):
        buf += line + '\n'
        try:
            compile(buf, 'x', 'exec')
        except SyntaxError:
            continue
        exec(buf, {'mut': mut})
        buf = ''

bad = 0
for name, old, new, file in muts:
    t = (src / file).read_text()
    n = t.count(old)
    if n != 1 or old == new:
        bad += 1
        print(f'{name}: anchor occurs {n} times in {file}' + (' (new == old)' if old == new else ''))
print(len(muts), 'mutants,', bad, 'bad anchors')
