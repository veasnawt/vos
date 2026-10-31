#!/usr/bin/env python3
"""A3: compare two raw QEMU screendumps (P6 PPM) of the same UEFI boot and explain every differing pixel.

The two screens show the boot log of different builds. GRUB places the multiboot2 information block and the module at addresses that
depend on the ISO, so a few lines of the log differ in their digits. This script (1) counts the differing pixels, (2) groups the differing
pixel rows into text rows, (3) decodes the text of each such row from the screendump with the kernel's own 8x16 font table, and (4) checks
that every differing text row is a line whose serial text also differs, and that no row differs without one. Read-only; no VOS code runs.

usage: screenshot-compare.py BASELINE.ppm CANDIDATE.ppm font8x16.c BASELINE-serial.txt CANDIDATE-serial.txt
"""
import collections
import re
import sys
from pathlib import Path


def ppm(p):
    d = Path(p).read_bytes()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', d)
    return int(m[1]), int(m[2]), d[m.end():]


def main():
    a_path, b_path, font_path, sa_path, sb_path = sys.argv[1:6]
    glyphs = {tuple(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', r)): chr(0x20 + i)
              for i, r in enumerate(re.findall(r'\{\s*((?:0x[0-9A-Fa-f]{2},?\s*){16})\}', Path(font_path).read_text()))}
    wa, ha, A = ppm(a_path)
    wb, hb, B = ppm(b_path)
    assert (wa, ha) == (wb, hb), 'the screens must have the same size'
    w, h = wa, ha
    bg = collections.Counter(tuple(A[i:i + 3]) for i in range(0, len(A), 3)).most_common(1)[0][0]
    rows = [y for y in range(h) if A[y * w * 3:(y + 1) * w * 3] != B[y * w * 3:(y + 1) * w * 3]]
    ndiff = sum(1 for y in rows for x in range(w) if A[(y * w + x) * 3:(y * w + x) * 3 + 3] != B[(y * w + x) * 3:(y * w + x) * 3 + 3])
    clusters = []
    for y in rows:
        if clusters and y - clusters[-1][1] <= 4:
            clusters[-1][1] = y
        else:
            clusters.append([y, y])

    def text(P, top):
        out = ''
        for c in range(w // 8):
            pat = []
            for r in range(16):
                v = 0
                for b in range(8):
                    i = ((top + r) * w + c * 8 + b) * 3
                    v = (v << 1) | (1 if tuple(P[i:i + 3]) != bg else 0)
                pat.append(v)
            out += glyphs.get(tuple(pat), '?')
        return out.rstrip()

    sa, sb = Path(sa_path).read_text().splitlines(), Path(sb_path).read_text().splitlines()
    changed_serial = [(x, y) for x, y in zip(sa, sb) if x != y]
    print(f'{Path(a_path).name} vs {Path(b_path).name}: {w}x{h}; differing pixels {ndiff} of {w * h}; differing pixel rows {len(rows)}; text rows with a difference {len(clusters)}')
    print(f'serial lines whose text differs between the two boots: {len(changed_serial)}')
    covered = 0
    for lo, hi in clusters:
        ta, tb = text(A, lo - 3), text(B, lo - 3)
        explained = any(ta.strip() and ta.strip() in x.strip() or ta.strip() == x.strip() for x, _ in changed_serial)
        covered += explained
        print(f'  y {lo}-{hi}  baseline: {ta!r}\n            candidate: {tb!r}   -> {"explained by a serial line that differs" if explained else "UNEXPLAINED"}')
    unexplained = len(clusters) - covered
    print(f'unexplained pixel rows: {unexplained}')
    sys.exit(0 if unexplained == 0 else 1)


if __name__ == '__main__':
    main()
