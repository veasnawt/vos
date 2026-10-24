#!/usr/bin/env python3
"""Deterministic, private 8 MiB derivative; never invokes the old fixture generator.

Only the pinned classic ext2 profile is supported. Allocation and layout are
explicit so deep lookup and directory-cookie costs do not depend on debugfs.
Run e2fsck -fn independently before admitting the resulting image to QEMU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BASE_SHA = 'b73a379294278454842d2dce7de93176ba6c32e730b6ea6dd47ab59c9e48e11b'
SIZE = 8 * 1024 * 1024
STAMP = 1700000000
SEED = 0x4E360001


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def pattern(role, size):
    return bytes(((SEED >> (8 * (j & 3))) + 73 * role + 29 * (j // 4096)
                  + 17 * (j % 4096) + ((j % 4096) >> 8)) & 255 for j in range(size))


class Image:
    def __init__(self, data):
        self.data = bytearray(data)
        self.bs = 1024 << self.u32(1048)
        self.blocks, self.inodes = self.u32(1028), self.u32(1024)
        self.first, self.bpg, self.ipg = self.u32(1044), self.u32(1056), self.u32(1064)
        self.isize = self.u16(1112)
        self.groups = (self.blocks - self.first + self.bpg - 1) // self.bpg
        need(len(data) == SIZE and self.bs == 1024 and self.blocks == 8192 and
             self.inodes == 512 and self.isize == 256 and self.groups == 8 and
             self.u16(1080) == 0xEF53, 'unexpected fixture geometry')
        need(self.u32(1120) == 2 and not self.u32(1116) & 4, 'unsupported ext2 features')
        self.new_blocks, self.new_inodes = [], []

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def put16(self, offset, value):
        struct.pack_into('<H', self.data, offset, value)

    def put32(self, offset, value):
        struct.pack_into('<I', self.data, offset, value)

    def gd(self, group):
        return 2048 + 32 * group

    def inode(self, number):
        need(1 <= number <= self.inodes, 'inode out of range')
        group, index = divmod(number - 1, self.ipg)
        return self.u32(self.gd(group) + 8) * self.bs + index * self.isize

    def allocated(self, number, inode=False):
        origin, per, field = (1, self.ipg, 4) if inode else (self.first, self.bpg, 0)
        group, index = divmod(number - origin, per)
        need(0 <= group < self.groups, 'bitmap group out of range')
        p = self.u32(self.gd(group) + field) * self.bs + index // 8
        return bool(self.data[p] & (1 << (index & 7)))

    def alloc(self, inode=False, directory=False):
        origin, maximum, per, field = ((11, self.inodes + 1, self.ipg, 4) if inode
                                       else (self.first, self.blocks, self.bpg, 0))
        for number in range(origin, maximum):
            if self.allocated(number, inode):
                continue
            group, index = divmod(number - (1 if inode else self.first), per)
            gd = self.gd(group)
            p = self.u32(gd + field) * self.bs + index // 8
            self.data[p] |= 1 << (index & 7)
            count = gd + (14 if inode else 12)
            self.put16(count, self.u16(count) - 1)
            count = 1040 if inode else 1036
            self.put32(count, self.u32(count) - 1)
            if directory:
                self.put16(gd + 16, self.u16(gd + 16) + 1)
            (self.new_inodes if inode else self.new_blocks).append(number)
            if inode:
                start, length = self.inode(number), self.isize
            else:
                start, length = number * self.bs, self.bs
            self.data[start:start + length] = bytes(length)
            return number
        raise ValueError('8 MiB fixture has insufficient free allocation')

    def block_bytes(self, block):
        need(0 < block < self.blocks and self.allocated(block), 'unallocated data/map block')
        return bytes(self.data[block * self.bs:(block + 1) * self.bs])

    def mappings(self, ino):
        p = self.inode(ino)
        size = self.u32(p + 4)
        count = (size + self.bs - 1) // self.bs
        need(count <= 12 + 256 + 256 * 256, 'unsupported triple-indirect fixture object')
        data, maps = [], []
        for n in range(count):
            if n < 12:
                block = self.u32(p + 40 + n * 4)
            elif n < 268:
                root = self.u32(p + 88)
                self.block_bytes(root)
                if root not in maps:
                    maps.append(root)
                block = self.u32(root * self.bs + (n - 12) * 4)
            else:
                root = self.u32(p + 92)
                self.block_bytes(root)
                if root not in maps:
                    maps.append(root)
                leaf = self.u32(root * self.bs + ((n - 268) // 256) * 4)
                self.block_bytes(leaf)
                if leaf not in maps:
                    maps.append(leaf)
                block = self.u32(leaf * self.bs + ((n - 268) % 256) * 4)
            self.block_bytes(block)
            data.append(block)
        return data, maps

    def content(self, ino):
        blocks, _ = self.mappings(ino)
        return b''.join(self.block_bytes(b) for b in blocks)[:self.u32(self.inode(ino) + 4)]

    def records(self, ino):
        need(self.u16(self.inode(ino)) & 0xF000 == 0x4000, 'not a directory')
        data = self.content(ino)
        records = []
        need(len(data) % self.bs == 0, 'partial directory block')
        for start in range(0, len(data), self.bs):
            pos = start
            while pos < start + self.bs:
                target, length, nlen, kind = struct.unpack_from('<IHBB', data, pos)
                need(length >= 8 and length % 4 == 0 and pos + length <= start + self.bs
                     and nlen <= length - 8, 'invalid directory record')
                name = data[pos + 8:pos + 8 + nlen]
                if target:
                    need(self.allocated(target, True), 'directory references unallocated inode')
                    mode = self.u16(self.inode(target)) & 0xF000
                    need(kind == {0x8000: 1, 0x4000: 2, 0x2000: 3, 0x6000: 4,
                                  0x1000: 5, 0xC000: 6, 0xA000: 7}.get(mode),
                         'inode/type mismatch')
                    need(name and b'/' not in name and b'\0' not in name, 'invalid directory name')
                records.append((pos, target, length, name, kind))
                pos += length
        names = [r[3] for r in records if r[1]]
        need(len(names) == len(set(names)), 'duplicate directory name')
        return records

    def resolve(self, path):
        ino = 2
        for name in path.strip('/').split('/'):
            if not name:
                continue
            found = [r[1] for r in self.records(ino) if r[1] and r[3] == name.encode()]
            need(len(found) == 1, 'path not uniquely present: ' + path)
            ino = found[0]
        return ino

    def entry(self, block, offset, ino, name, kind, length):
        name = name.encode() if isinstance(name, str) else name
        need(length >= (8 + len(name) + 3) // 4 * 4, 'entry too small')
        p = block * self.bs + offset
        self.data[p:p + length] = bytes(length)
        struct.pack_into('<IHBB', self.data, p, ino, length, len(name), kind)
        self.data[p + 8:p + 8 + len(name)] = name

    def insert(self, parent, child, name, kind):
        blocks, _ = self.mappings(parent)
        required = (8 + len(name) + 3) // 4 * 4
        for pos, ino, length, oldname, _ in self.records(parent):
            used = (8 + len(oldname) + 3) // 4 * 4 if ino else 0
            if length - used < required:
                continue
            block, off = blocks[pos // self.bs], pos % self.bs
            if used:
                self.put16(block * self.bs + off + 4, used)
            self.entry(block, off + used, child, name, kind, length - used)
            if kind == 2:
                p = self.inode(parent) + 26
                self.put16(p, self.u16(p) + 1)
            return
        raise ValueError('no admitted parent directory slack')

    def create(self, parent, name, payload=None, count=1):
        directory = payload is None
        ino = self.alloc(inode=True, directory=directory)
        p = self.inode(ino)
        self.put16(p, 0o40755 if directory else 0o100444)
        self.put16(p + 26, 2 if directory else 1)
        for offset in [8, 12, 16]:
            self.put32(p + offset, STAMP)
        size = count * self.bs if directory else len(payload)
        self.put32(p + 4, size)
        data = [self.alloc() for _ in range((size + self.bs - 1) // self.bs)]
        maps = []
        for i, block in enumerate(data):
            if i < 12:
                self.put32(p + 40 + i * 4, block)
            elif i < 268:
                if i == 12:
                    root = self.alloc(); maps.append(root); self.put32(p + 88, root)
                self.put32(self.u32(p + 88) * self.bs + (i - 12) * 4, block)
            else:
                if i == 268:
                    root = self.alloc(); maps.append(root); self.put32(p + 92, root)
                root = self.u32(p + 92)
                if (i - 268) % 256 == 0:
                    leaf = self.alloc(); maps.append(leaf)
                    self.put32(root * self.bs + ((i - 268) // 256) * 4, leaf)
                leaf = self.u32(root * self.bs + ((i - 268) // 256) * 4)
                self.put32(leaf * self.bs + ((i - 268) % 256) * 4, block)
        self.put32(p + 28, (len(data) + len(maps)) * 2)
        if directory:
            for block in data:
                self.entry(block, 0, 0, '', 0, self.bs)
            self.entry(data[0], 0, ino, '.', 2, 12)
            self.entry(data[0], 12, parent, '..', 2, self.bs - 12)
        else:
            for i, block in enumerate(data):
                chunk = payload[i * self.bs:(i + 1) * self.bs]
                self.data[block * self.bs:block * self.bs + len(chunk)] = chunk
        self.insert(parent, ino, name, 2 if directory else 1)
        return ino

    def synchronize_backups(self):
        # sparse_super profile: groups 1,3,5,7 have backup super/GDT blocks.
        for group in [1, 3, 5, 7]:
            p = (self.first + group * self.bpg) * self.bs
            for field in [12, 16]:
                self.put32(p + field, self.u32(1024 + field))
            self.data[p + self.bs:p + self.bs + self.groups * 32] = self.data[2048:2048 + self.groups * 32]


def many_manifest(image):
    ino = image.resolve('/many')
    names = []
    for pos, child, length, name, kind in image.records(ino):
        if child and name not in (b'.', b'..'):
            names.append(dict(name=name.decode(), inode=child, offset=pos, length=length,
                              kind=kind, sha256=sha(image.content(child))))
    need(len(names) == 200, 'original /many count changed')
    return dict(inode=ino, layout_sha256=sha(image.content(ino)), entries=names)


def build(source):
    original = source.read_bytes()
    need(sha(original) == BASE_SHA, 'unapproved source fixture')
    old = Image(original)
    many = many_manifest(old)
    image = Image(original)
    scope = image.create(2, 'scope')
    expected = {}
    for role in range(2):
        directory = image.create(scope, 'r' + str(role))
        payload = pattern(role, 17 * 4096)
        image.create(directory, 'data.bin', payload)
        expected['/scope/r%d/data.bin' % role] = payload
    deep = image.create(scope, 'deep')
    for depth in range(1, 65):
        deep = image.create(deep, 'd%02d' % depth)
    errors = image.create(scope, 'errors')
    payload = pattern(2, 12 * 1024)
    image.create(errors, 'prefix.bin', payload)
    expected['/scope/errors/prefix.bin'] = payload
    tiny = image.create(scope, 'canary')
    image.create(tiny, 'only', b'canary\n')
    expected['/scope/canary/only'] = b'canary\n'
    huge = image.create(scope, 'huge', count=512)
    tail = image.create(scope, 'tail', b'tail\n')
    expected['/scope/tail'] = b'tail\n'
    # Move its single link from /scope into the final large-directory block.
    for pos, child, length, name, kind in image.records(scope):
        if name == b'tail':
            blocks, _ = image.mappings(scope)
            image.entry(blocks[pos // 1024], pos % 1024, 0, '', 0, length)
    blocks, maps = image.mappings(huge)
    image.entry(blocks[-1], 0, tail, 'tail', 1, 1024)
    expected['/scope/huge/tail'] = expected.pop('/scope/tail')
    image.synchronize_backups()
    need(many_manifest(image) == many, '/many differs')
    # Old allocated inode bytes remain exact, except root's new name/link count.
    for ino in range(1, old.inodes + 1):
        if ino != 2 and old.allocated(ino, True):
            p = old.inode(ino)
            need(image.data[p:p + old.isize] == old.data[p:p + old.isize], 'original inode changed')
    need(len(set(image.new_blocks)) == len(image.new_blocks) and
         all(not old.allocated(b) for b in image.new_blocks), 'new/old allocation collision')
    for g in range(image.groups):
        begin = image.first + g * image.bpg
        free = sum(not image.allocated(b) for b in range(begin, min(begin + image.bpg, image.blocks)))
        ifree = sum(not image.allocated(i, True) for i in range(1 + g * image.ipg, 1 + (g + 1) * image.ipg))
        need(free == image.u16(image.gd(g) + 12) and ifree == image.u16(image.gd(g) + 14), 'bitmap/count disagreement')
    files = {}
    for path, content in expected.items():
        ino = image.resolve(path)
        data, indirect = image.mappings(ino)
        need(image.content(ino) == content, 'file content mismatch')
        need(image.u32(image.inode(ino) + 28) == 2 * (len(data) + len(indirect)), 'i_blocks mismatch')
        files[path] = dict(inode=ino, size=len(content), sha256=sha(content), blocks=data,
                           indirect=indirect, region_sha256=[sha(content[i:i + 4096]) for i in range(0, len(content), 4096)])
    for ino in image.new_inodes:
        if image.u16(image.inode(ino)) & 0xF000 == 0x4000:
            image.records(ino)
    chain = image.resolve('/scope/deep')
    for depth in range(1, 65):
        child = image.resolve('/scope/deep/' + '/'.join('d%02d' % n for n in range(1, depth + 1)))
        need(len(image.mappings(child)[0]) == 1 and not image.mappings(child)[1], 'deep directory not direct/one-block')
        need(image.records(child)[1][1] == chain, 'deep parent mismatch')
        chain = child
    live = [(p, n) for p, ino, length, n, kind in image.records(huge) if ino and n not in (b'.', b'..')]
    need(live == [(523264, b'tail')] and len(blocks) == 512 and len(maps) == 3, 'huge layout mismatch')
    data = bytes(image.data)
    manifest = dict(version=1, source_sha256=BASE_SHA, sha256=sha(data), logical_size=len(data),
                    block_size=1024, new_blocks=len(image.new_blocks), new_inodes=len(image.new_inodes),
                    free_blocks=image.u32(1036), free_inodes=image.u32(1040), many=many, files=files,
                    deep=dict(depth=64, logical_formula='2+9*depth', components='d01 through d64'),
                    huge=dict(inode=huge, blocks=blocks, indirect=maps, near_end_cookie=523264, eof_cookie=524288))
    return data, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    need(not args.output.exists() and not args.manifest.exists(), 'refusing to overwrite outputs')
    data, manifest = build(args.source)
    with args.output.open('xb') as f:
        f.write(data)
    args.output.chmod(0o444)
    with args.manifest.open('x') as f:
        f.write(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    need(sha(args.source.read_bytes()) == BASE_SHA, 'source changed during construction')
    print(json.dumps({k: manifest[k] for k in ['sha256', 'logical_size', 'new_blocks', 'new_inodes', 'free_blocks', 'free_inodes']}))


if __name__ == '__main__':
    main()
