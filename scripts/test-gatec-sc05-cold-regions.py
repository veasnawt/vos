#!/usr/bin/env python3
"""Host test: SC05's fault regions are cold after setup, checked on the actual disk image; no build, no VOS execution.

SC05 invalidates the mock device before victim setup, and setup's path walk then warms directory, inode and other
metadata cache lines. Each faulted read must miss in every line it touches, or the per-sector fallback reads fewer
sectors than the test requires. A region is cold when every block of every cache line it touches is a data block of
that same file, so no other read (metadata or another file) can load the line. Execution 1 failed because region 0 of
r0/data.bin shares a line with the /scope and /scope/r0 directory blocks.

Checked here:
  * the whole ext2 tree's block ownership, with no block claimed twice;
  * the cache line size from blockdev.h (BLK_LINE_SECTORS x BLK_SECTOR_SIZE);
  * every region storage_stress_c.c uses (SC05_REGION over its 12 cases; SC05_HOLDER_REGION for the waiter roles),
    which must be cold and direct;
  * the SC05 assertions that depend on this: exactly 8 single-sector reads for the fallback, and 2 then 1 for the
    prefix. Both follow from the backend contract: a failed line load falls back to its requested sectors one by
    one, uncached, and ext2 reads one block per request with no retry.

usage: test-gatec-sc05-cold-regions.py IMAGE [unittest options]
"""
from pathlib import Path
import re
import struct
import sys
from unittest import TestCase, main

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'arch/x86_64'
FILES = {'victim': '/scope/r0/data.bin', 'holder': '/scope/r1/data.bin'}
BLOCKS_PER_REGION = 4                           # one 4096-byte read at region*4096, in 1 KiB blocks
IMAGE = None


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def ext2_layout(d):
    """Block ownership of every inode reachable from the root: 'data:', 'dir:' or 'indirect:' plus the path."""
    sb = d[1024:2048]
    if struct.unpack_from('<H', sb, 56)[0] != 0xEF53:
        raise ValueError('not an ext2 image')
    bs = 1024 << u32(sb, 24)
    ipg = u32(sb, 40)
    isz = struct.unpack_from('<H', sb, 88)[0] if u32(sb, 76) >= 1 else 128
    gdt = (u32(sb, 20) + 1) * bs

    def inode(n):
        g, i = (n - 1) // ipg, (n - 1) % ipg
        off = u32(d, gdt + 32 * g + 8) * bs + i * isz
        return d[off:off + isz]

    def mapping(raw):
        ptr = struct.unpack_from('<15I', raw, 40)
        data, meta = [], []

        def walk(b, depth):
            if not b:
                return
            if depth == 0:
                data.append(b)
                return
            meta.append(b)
            for p in struct.unpack_from('<%dI' % (bs // 4), d, b * bs):
                walk(p, depth - 1)
        for b in ptr[:12]:
            walk(b, 0)
        for depth, b in zip((1, 2, 3), ptr[12:]):
            walk(b, depth)
        return data, meta

    owner, conflicts, seen, files = {}, [], set(), {}

    def claim(b, who):
        if b in owner and owner[b] != who:
            conflicts.append((b, owner[b], who))
        owner.setdefault(b, who)

    def visit(n, path):
        if n in seen:
            return
        seen.add(n)
        raw = inode(n)
        kind = struct.unpack_from('<H', raw, 0)[0] >> 12
        if kind == 0xA and u32(raw, 4) < 60:
            return                                  # fast symlink: the target lives in the inode
        data, meta = mapping(raw)
        files[path] = data
        for b in meta:
            claim(b, 'indirect:' + path)
        for b in data:
            claim(b, ('dir:' if kind == 4 else 'data:') + path)
        if kind == 4:
            for b in data:
                e = b * bs
                while e < b * bs + bs:
                    i, rl, nl = struct.unpack_from('<IHB', d, e)
                    if rl < 8:
                        raise ValueError('malformed directory block %d' % b)
                    name = d[e + 8:e + 8 + nl].decode()
                    if i and name not in ('.', '..'):
                        visit(i, path.rstrip('/') + '/' + name)
                    e += rl

    visit(2, '/')
    return dict(block_bytes=bs, owner=owner, conflicts=conflicts, files=files)


def line_bytes(src=SRC):
    h = (Path(src) / 'blockdev.h').read_text()
    sectors = int(re.search(r'#define BLK_LINE_SECTORS\s+(\d+)', h)[1])
    size = int(re.search(r'#define BLK_SECTOR_SIZE\s+(\d+)', h)[1])
    return sectors * size


def region(layout, path, r, line):
    """Blocks, cache lines and every foreign block in those lines for region r of path."""
    bs = layout['block_bytes']
    data = layout['files'][path]
    blocks = data[BLOCKS_PER_REGION * r:BLOCKS_PER_REGION * (r + 1)]
    per = line // bs
    lines = sorted({b // per for b in blocks})
    foreign = [(b, layout['owner'].get(b)) for l in lines for b in range(l * per, (l + 1) * per)
               if layout['owner'].get(b) != 'data:' + path]
    return dict(blocks=blocks, lines=lines, foreign=foreign,
                direct=BLOCKS_PER_REGION * (r + 1) <= 12 and len(blocks) == BLOCKS_PER_REGION)


def kernel_regions(src=SRC):
    """The regions storage_stress_c.c's SC05 uses, evaluated from its own macros over its 12-case loop."""
    text = (Path(src) / 'storage_stress_c.c').read_text()
    defs = {}
    for name, arg in (('SC05_REGION', 'n'), ('SC05_HOLDER_REGION', 'r')):
        m = re.search(r'#define %s\(%s\)\s+(\(.*?\))\s*(?:/\*|$)' % (name, arg), text, re.M)
        if not m:
            raise AssertionError(name + ' missing')
        expr = re.sub(r'\b(\d+)u\b', r'\1', m[1])
        if not re.fullmatch('[0-9' + arg + r'()+\-*/% ]+', expr):
            raise AssertionError(name + ' is not a plain arithmetic expression')
        defs[name] = (arg, expr.replace('/', '//'))
    ev = lambda name, v: eval(defs[name][1], {'__builtins__': {}}, {defs[name][0]: v})
    if not re.search(r'for\(unsigned round=0;round<2;round\+\+\) for\(unsigned fault=0;fault<3;fault\+\+\) '
                     r'for\(unsigned role=0;role<2;role\+\+\)\n\s+sc05_case\(bi,n\+\+,fault,role,round\);', text):
        raise AssertionError('SC05 loop order changed: case n no longer means round*6+fault*2+role')
    victim = {n: ev('SC05_REGION', n) for n in range(12)}
    holder = {n: ev('SC05_HOLDER_REGION', victim[n]) for n in range(12) if n % 2 == 1}     # role waiter = n odd
    return victim, holder


def contract_problems(src=SRC):
    text = (Path(src) / 'storage_stress_c.c').read_text()
    case = text[text.index('static void sc05_case('):text.index('static void sc05(')]
    need = {
        'region from SC05_REGION': '    unsigned region=SC05_REGION(n);',
        'holder from SC05_HOLDER_REGION': 'victim(bi,H,OP_READ_R1,1,SC05_HOLDER_REGION(region),0)',
        'invalidation before setup': 'require(!blk_invalidate(dev),"cold device before setup',
        'SC05 fallback must require exactly 8 single-sector reads': 'md.multi_failed && md.single_ok==8 && !md.single_failed',
        'SC05 prefix must require exactly 2 then 1 single-sector reads': 'md.single_ok==2 && md.single_failed==1',
    }
    found = [what for what, s in need.items() if s not in case]
    if 'invalidation before setup' not in found and \
            case.index('require(!blk_invalidate(dev),"cold device before setup') > case.index('victim(bi,V,OP_READ,2,region,0)'):
        found.append('invalidation before setup')
    return found


def cold_problems(layout, line, victim, holder):
    found = ['block %d claimed twice (%s, %s)' % c for c in layout['conflicts']]
    for role, regions in (('victim', victim), ('holder', holder)):
        path = FILES[role]
        for r in sorted(set(regions.values())):
            info = region(layout, path, r, line)
            if not info['direct']:
                found.append('%s region %d is not four direct blocks' % (path, r))
            if info['foreign']:
                found.append('%s region %d is not cold: lines %s also hold %s' % (path, r, info['lines'], info['foreign'][:4]))
    return found


class Synthetic(TestCase):
    """The checks themselves, on a hand-made layout (1 KiB blocks, 4 KiB lines)."""
    def layout(self, owner):
        files = {}
        for b, who in sorted(owner.items()):
            if who.startswith('data:'):
                files.setdefault(who[5:], []).append(b)
        return dict(block_bytes=1024, owner=owner, conflicts=[], files=files)

    def test_cold_when_lines_hold_only_the_file(self):
        owner = {b: 'data:' + FILES['victim'] for b in range(8, 24)}
        owner.update({b: 'data:' + FILES['holder'] for b in range(24, 40)})
        self.assertEqual(cold_problems(self.layout(owner), 4096, {0: 1}, {1: 1}), [])

    def test_directory_block_in_a_region_line_is_detected(self):
        owner = {b: 'data:' + FILES['victim'] for b in range(9, 24)}
        owner[8] = 'dir:/scope/r0'
        owner.update({b: 'data:' + FILES['holder'] for b in range(24, 40)})
        self.assertEqual(len(cold_problems(self.layout(owner), 4096, {0: 0}, {})), 1)
        self.assertEqual(cold_problems(self.layout(owner), 4096, {0: 1}, {}), [])

    def test_unowned_or_foreign_block_is_detected(self):
        owner = {b: 'data:' + FILES['victim'] for b in range(8, 24) if b != 13}
        self.assertIn('not cold', cold_problems(self.layout(owner), 4096, {0: 1}, {})[0])

    def test_conflict_and_indirect_region_are_detected(self):
        layout = self.layout({b: 'data:' + FILES['victim'] for b in range(8, 24)})
        layout['conflicts'] = [(9, 'dir:/x', 'data:/y')]
        self.assertIn('claimed twice', cold_problems(layout, 4096, {0: 1}, {})[0])
        fresh = self.layout({b: 'data:' + FILES['victim'] for b in range(8, 24)})
        self.assertIn('not four direct blocks', cold_problems(fresh, 4096, {0: 3}, {})[0])


class Fixture(TestCase):
    """The admitted disk image and the kernel's SC05 regions and assertions."""
    @classmethod
    def setUpClass(cls):
        cls.layout = ext2_layout(Path(IMAGE).read_bytes())
        cls.line = line_bytes()

    def test_geometry(self):
        self.assertEqual((self.layout['block_bytes'], self.line), (1024, 4096))
        self.assertEqual(self.layout['conflicts'], [])

    def test_kernel_regions_are_cold(self):
        victim, holder = kernel_regions()
        # The image check first, so a warm region is reported by the blocks that warm it.
        self.assertEqual(cold_problems(self.layout, self.line, victim, holder), [])
        self.assertEqual(sorted(set(victim.values())), [1, 2])
        self.assertEqual(sorted(set(holder.values())), [1, 2])
        self.assertTrue(all(holder[n] != victim[n] for n in holder))
        # Each fault x role runs in both cold regions (round 0 and round 1), so coverage is not reduced.
        self.assertTrue(all(victim[n] != victim[n + 6] for n in range(6)))
        self.assertTrue(all(holder[n] != holder[n + 6] for n in holder if n < 6))

    def test_execution_1_defect_is_visible(self):
        # Positive control: region 0 of each file shares a line with that file's directory block.
        for path, d in ((FILES['victim'], 'dir:/scope/r0'), (FILES['holder'], 'dir:/scope/r1')):
            self.assertIn(d, [o for _, o in region(self.layout, path, 0, self.line)['foreign']])

    def test_kernel_assertions_match_the_cold_contract(self):
        self.assertEqual(contract_problems(), [])


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1].startswith('-'):
        sys.exit(__doc__.strip().splitlines()[-1])
    IMAGE = sys.argv.pop(1)
    main()
