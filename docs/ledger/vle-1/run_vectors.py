#!/usr/bin/env python3
"""VLE-1 conformance runner. Language-neutral: it runs any verifier command (SPEC.md section 15) against fixtures/VECTORS.json.
For each vector it copies fixtures/log to a fresh temporary directory, applies the vector's edits there (never to the fixtures),
runs the verifier and compares its single output line, parsed as JSON, with the expected result exactly.
usage: run_vectors.py <handoff-dir> -- <verifier command...>     e.g.  run_vectors.py . -- ./vle1verify
Exit 0 only if every vector matches. Prints one line per vector and a summary."""
import json, os, shutil, subprocess, sys, tempfile

def main():
    if '--' not in sys.argv: raise SystemExit(__doc__)
    root = os.path.abspath(sys.argv[1]); cmd = sys.argv[sys.argv.index('--') + 1:]
    fx = os.path.join(root, 'fixtures'); vectors = json.load(open(os.path.join(fx, 'VECTORS.json')))['vectors']
    failed = []
    for v in vectors:
        with tempfile.TemporaryDirectory() as tmp:
            log = os.path.join(tmp, 'log'); shutil.copytree(os.path.join(fx, 'log'), log)
            for e in v['edits']:
                if 'delete' in e: os.unlink(os.path.join(log, e['delete']))
                elif 'write' in e:
                    with open(os.path.join(log, e['write']), 'wb') as f: f.write(e['text'].encode('utf-8'))
                elif 'copy' in e: shutil.copyfile(os.path.join(log, e['copy'][0]), os.path.join(log, e['copy'][1]))
                elif 'swap' in e:
                    a, b = (os.path.join(log, x) for x in e['swap']); t = a + '.swap'; os.rename(a, t); os.rename(b, a); os.rename(t, b)
                elif 'flip' in e:
                    p = os.path.join(log, e['flip']); data = bytearray(open(p, 'rb').read()); data[e['offset']] ^= 0x01
                    open(p, 'wb').write(bytes(data))
                else: raise SystemExit('unknown edit %r' % e)
            args = [a.replace('{log}', log).replace('{fixtures}', fx) for a in v['args']]
            p = subprocess.run(cmd + args, capture_output=True, text=True, timeout=120)
            try:
                lines = p.stdout.splitlines(); got = json.loads(lines[0]) if p.returncode == 0 and len(lines) == 1 else None
            except ValueError: got = None
            ok = got == v['expect']
            print('%-4s %-44s %s' % ('ok' if ok else 'FAIL', v['id'], '' if ok else 'expected %s got %s (exit %d) %s' % (json.dumps(v['expect']), p.stdout.strip()[:300], p.returncode, p.stderr.strip()[:200])))
            if not ok: failed.append(v['id'])
    print('VLE-1 CONFORMANCE %d/%d passed%s' % (len(vectors) - len(failed), len(vectors), '' if not failed else '; failed: ' + ', '.join(failed)))
    return 1 if failed else 0

if __name__ == '__main__': sys.exit(main())
