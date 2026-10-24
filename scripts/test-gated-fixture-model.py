#!/usr/bin/env python3
"""Host test: Gate D's SC10/SC11 expectations, recomputed from the actual disk image; no build, no VOS execution.

An independent model of the source's charging rules (one logical unit per ext2 device access, cache hits
included; the 513th access of a 512-unit scope fails) and of the sectors each access reads:
  inode load = inode-bitmap sector + inode-table sector; directory block = bmap indirect sectors + the block;
  ext2_lookup = dir inode + step's dir inode + scanned blocks + child inode; open = base inode + per component
  (lookup + child inode); next entry = dir inode + per readdir call (dir inode + blocks from the cookie's block)
  + entry inode.
Checked:
  * SC10 open costs are 2+9d on the deep chain and the runner's and kernel's depths agree;
  * SC11 /many: 200 entries (file-001..file-200 in record order) then EOF, 1408 units per pass; every huge probe's
    result and cost match the runner's table, and the kernel probes the same cookies;
  * warm expectations are justified: every cold operation touches at most BLK_CACHE_LINES distinct 4 KiB lines
    (a /many pass together with a program's setup), and the depth-56 recovery walk only touches lines its
    preceding depth-57 attempt loaded;
  * the sparse-directory limitation the probes document: from cookie 0 no call can make progress in
    /scope/huge within the budget (EAGAIN carries no cookie), and block 344 is the first start that can.

usage: test-gated-fixture-model.py IMAGE [unittest options]
"""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
import struct
import sys
from unittest import TestCase, main

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / 'arch/x86_64'
LIMIT = 512
IMAGE = None


class Fixture:
    def __init__(self, data):
        self.d = data
        sb = data[1024:2048]
        self.bs = 1024 << self.u32(1024 + 24)
        self.spb = self.bs // 512
        self.ipg, self.isz = self.u32(1024 + 40), self.u16(1024 + 88)
        self.gdt = (self.u32(1024 + 20) + 1) * self.bs
        self.ptrs = self.bs // 4
        assert sb[56:58] == b'\x53\xef'

    def u32(self, o):
        return struct.unpack_from('<I', self.d, o)[0]

    def u16(self, o):
        return struct.unpack_from('<H', self.d, o)[0]

    def inode_load(self, ino):
        """(inode offset, the two sectors read: bitmap and inode table)."""
        g, i = (ino - 1) // self.ipg, (ino - 1) % self.ipg
        bitmap, table = self.u32(self.gdt + 32 * g + 4), self.u32(self.gdt + 32 * g + 8)
        byte = i * self.isz
        return (table * self.bs + byte, [bitmap * self.spb + (i // 8) // 512,
                                         (table + byte // self.bs) * self.spb + (byte % self.bs) // 512])

    def size(self, ino):
        return self.u32(self.inode_load(ino)[0] + 4)

    def bmap(self, ino, lb):
        """(physical block, indirect sectors read)."""
        off = self.inode_load(ino)[0] + 40
        if lb < 12:
            return self.u32(off + 4 * lb), []
        lb -= 12
        if lb < self.ptrs:
            b = self.u32(off + 48)
            return self.u32(b * self.bs + 4 * lb), [b * self.spb + (4 * lb) // 512]
        lb -= self.ptrs
        b1 = self.u32(off + 52)
        b2 = self.u32(b1 * self.bs + 4 * (lb // self.ptrs))
        return (self.u32(b2 * self.bs + 4 * (lb % self.ptrs)),
                [b1 * self.spb + (4 * (lb // self.ptrs)) // 512, b2 * self.spb + (4 * (lb % self.ptrs)) // 512])

    def block_read(self, ino, lb):
        pb, ind = self.bmap(ino, lb)
        return pb, [[s] for s in ind] + [list(range(pb * self.spb, pb * self.spb + self.spb))]

    def records(self, pb):
        out, p = [], 0
        while p < self.bs:
            e = pb * self.bs + p
            out.append((p, self.u16(e + 4), self.u32(e), self.d[e + 8:e + 8 + self.d[e + 6]].decode()))
            p += self.u16(e + 4)
        return out

    def lookup(self, dirino, name):
        acc = [[s] for s in self.inode_load(dirino)[1]] * 2      # api_ino(dir), step's inode_load(dir)
        for lb in range(self.size(dirino) // self.bs):
            pb, reads = self.block_read(dirino, lb)
            acc += reads
            for p, rl, ino, nm in self.records(pb):
                if ino and nm == name:
                    return ino, acc + [[s] for s in self.inode_load(ino)[1]]
        raise KeyError(name)

    def walk(self, base, path):
        acc, ino = [[s] for s in self.inode_load(base)[1]], base
        for c in path.split('/'):
            ino, a = self.lookup(ino, c)
            acc += a + [[s] for s in self.inode_load(ino)[1]]
        return ino, acc

    def next_entry(self, dirino, cookie):
        """(result, accesses, next, name): result 1 entry, 0 EOF, 'EINVAL'."""
        acc = [[s] for s in self.inode_load(dirino)[1]]
        size, pos = self.size(dirino), cookie
        if cookie > size:
            return 'EINVAL', acc, None, None
        while pos < size:
            acc += [[s] for s in self.inode_load(dirino)[1]]
            lb, want = pos // self.bs, pos % self.bs
            pb, reads = self.block_read(dirino, lb)
            acc += reads
            recs = self.records(pb)
            if want and want not in [p for p, _, _, _ in recs]:
                return 'EINVAL', acc, None, None
            found = None
            while True:
                found = next(((p, rl, ino, nm) for p, rl, ino, nm in recs if p >= want and ino), None)
                if found:
                    break
                lb, want = lb + 1, 0
                if lb * self.bs >= size:
                    return 0, acc, size, None
                pb, reads = self.block_read(dirino, lb)
                acc += reads
                recs = self.records(pb)
            p, rl, ino, nm = found
            pos = lb * self.bs + p + rl
            if nm in ('.', '..'):
                continue
            return 1, acc + [[s] for s in self.inode_load(ino)[1]], pos, nm
        return 0, acc, pos, None


def lines(accesses):
    """Distinct 4 KiB cache lines (8 sectors) touched by the accesses a 512-unit scope can perform."""
    return {s // 8 for a in accesses[:LIMIT] for s in a}


def load(name):
    spec = spec_from_file_location(name.replace('-', '_'), HERE / name)
    m = module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class Model(TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fx = Fixture(Path(IMAGE).read_bytes())
        cls.scope, _ = cls.fx.lookup(2, 'scope')
        cls.many, _ = cls.fx.lookup(2, 'many')
        cls.huge, _ = cls.fx.lookup(cls.scope, 'huge')
        sys.path.insert(0, str(HERE))
        cls.runner = load('test-storage-stress-d.py')
        cls.kernel = (SRC / 'storage_stress_d.c').read_text()
        h = (SRC / 'blockdev.h').read_text()
        cls.cache_lines = int(re.search(r'#define BLK_CACHE_LINES\s+(\d+)', h)[1])

    def chain(self, depth):
        return '/'.join(['deep'] + ['d%02d' % i for i in range(1, depth)])

    def test_sc10_costs_and_constants(self):
        self.assertEqual(tuple(self.runner.DEPTHS), (8, 56, 57, 64))
        self.assertIn('static const unsigned depths[4]={8,56,57,64};', self.kernel)
        for d in (1, 8, 56, 57, 64, 65):
            with self.subTest(depth=d):
                self.assertEqual(len(self.fx.walk(self.scope, self.chain(d))[1]), 2 + 9 * d)

    def test_sc11_many_pass(self):
        cookie, names, cost = 0, [], 0
        for k in range(201):
            r, acc, nxt, nm = self.fx.next_entry(self.many, cookie)
            cost += len(acc)
            if k < 200:
                self.assertEqual(r, 1)
                names.append(nm)
                cookie = nxt
            else:
                self.assertEqual((r, nxt), (0, 4096))
        self.assertEqual(names, ['file-%03d' % i for i in range(1, 201)])
        self.assertEqual(cost, self.runner.PASS_COST)

    def test_sc11_probes_match_runner_and_kernel(self):
        cookies = [p[0] for p in self.runner.PROBES]
        self.assertIn('static const uint64_t probes[10]={%s};' % ','.join(map(str, cookies)), self.kernel)
        for cookie, cost, result, name in self.runner.PROBES:
            with self.subTest(cookie=cookie):
                r, acc, nxt, nm = self.fx.next_entry(self.huge, cookie)
                want = 'EAGAIN' if len(acc) > LIMIT else {1: 'ENTRY', 0: 'EOF', 'EINVAL': 'EINVAL'}[r]
                self.assertEqual((len(acc), want), (cost, result))
                if result == 'ENTRY':
                    self.assertEqual((nm, nxt), (name, 524288))

    def test_warm_expectations_fit_the_cache(self):
        for d in (8, 56, 57, 64):
            with self.subTest(depth=d):
                touched = lines(self.fx.walk(self.scope, self.chain(d))[1])
                self.assertLessEqual(len(touched), self.cache_lines)
                self.assertGreater(len(touched), 0)
        self.assertLessEqual(lines(self.fx.walk(self.scope, self.chain(56))[1]), lines(self.fx.walk(self.scope, self.chain(57))[1]))
        setup = []
        for base, path in ((self.many, '.'), (self.scope, 'huge'), (self.scope, 'r0/data.bin'), (self.many, '.')):
            setup += self.fx.walk(base, path)[1] if path != '.' else [[s] for s in self.fx.inode_load(base)[1]]
        cookie, enum = 0, []
        for _ in range(201):
            r, acc, nxt, nm = self.fx.next_entry(self.many, cookie)
            enum += acc
            cookie = nxt
        self.assertLessEqual(len(lines(enum) | lines(setup)), self.cache_lines)

    def test_sparse_directory_limitation(self):
        size = self.fx.size(self.huge)
        firsts = [lb for lb in range(size // self.fx.bs) if len(self.fx.next_entry(self.huge, lb * self.fx.bs)[1]) <= LIMIT]
        self.assertEqual(firsts[0], 344)                     # starts before block 344 cannot advance within the budget
        self.assertGreater(len(self.fx.next_entry(self.huge, 0)[1]), LIMIT)


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1].startswith('-'):
        sys.exit(__doc__.strip().splitlines()[-1])
    IMAGE = sys.argv.pop(1)
    main()
