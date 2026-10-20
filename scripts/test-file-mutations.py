#!/usr/bin/env python3
"""Negative controls for N3.5: mutate fresh scratch copies, never the source tree.

Usage: python3 scripts/test-file-mutations.py SOURCE VERIFIED_BUILD OUTPUT
SOURCE is the Git worktree; VERIFIED_BUILD is its matching, built scratch tree.
OUTPUT must not exist. Needs the normal BIOS test dependencies. Each control
must finish with a concrete file assertion failure and the final FAIL verdict;
a panic, timeout, compiler error or a lost PASS marker is not detection.
"""
import pathlib
import os
import shutil
import stat
import subprocess
import sys
import tarfile


def run(*args, cwd, log):
    with log.open("w") as stream:
        subprocess.run(args, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, check=True)


def copy_artifact(source, destination):
    # Fixture source trees deliberately contain FIFOs and dangling symlinks.
    # Preserve their types; never open a FIFO or follow a fixture symlink.
    if stat.S_ISFIFO(os.stat(source, follow_symlinks=False).st_mode):
        os.mkfifo(destination)
        shutil.copystat(source, destination)
        return destination
    return shutil.copy2(source, destination)


def main():
    source, verified, output = (pathlib.Path(p).resolve() for p in sys.argv[1:])
    output.mkdir()  # refuse an existing target
    names = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=source
    ).decode().strip("\0").split("\0")
    # A manifest ensures the copied objects/fixtures belong to this source.
    for name in names:
        if name == "Makefile" or name.startswith(("arch/", "scripts/", "tests/", "user/")):
            assert (source / name).read_bytes() == (verified / name).read_bytes(), name
    controls = {
        "rights-width": ("arch/x86_64/file.c", "if (rights & ~FILE_RIGHTS)\n        return -VOS_EINVAL;",
                         "if ((uint32_t)rights & ~(uint32_t)FILE_RIGHTS)\n        return -VOS_EINVAL;",
                         "undefined rights rejected before I/O/publication/session reservation"),
        "symlink": ("arch/x86_64/file.c", "if (st->type == EXT2_T_LNK)\n                return -FILE_ELOOP;",
                    "if (0)\n                return -FILE_ELOOP;", "final and intermediate symlinks never followed"),
        "error-copy": ("arch/x86_64/file.c", "if (!r && !(r = outcome(s, EXT2_OK))) {",
                       "if (!(r = outcome(s, EXT2_OK))) {",
                       "I/O error after a successful private data chunk leaves all 4096 user bytes unchanged"),
        "incarnation": ("arch/x86_64/file.c", "d.incarnation != volume.incarnation)", "0)",
                        "never-reused incarnation rejects aliased device ID before any I/O"),
        "reserved-inode": ("arch/x86_64/ext2.c", "if (!entry_ino_ok(fs, le32(e)))",
                           "if (0)", "I4 corrupt metadata returns EIO with atomic output and reusable reader"),
    }
    archive = output / "source.tar"
    with tarfile.open(archive, "w") as tar:
        for name in sorted(set(names)):
            tar.add(source / name, arcname=name, recursive=False)
    for label, (name, original, replacement, assertion) in controls.items():
        target = output / label
        target.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(target, filter="data")
        shutil.copytree(verified / "build", target / "build", symlinks=True,
                        copy_function=copy_artifact)
        path = target / name
        text = path.read_text()
        assert text.count(original) == 1, label
        path.write_text(text.replace(original, replacement))
        run("make", "-j4", "ARCH=x86_64", "VOS_TEST=1", "VOS_TEST_CASE=ext2", "iso",
            cwd=target, log=output / (label + ".build.log"))
        # This runner checks guest exit 35, both kernel-only markers and no
        # triple fault. A timeout returns failure regardless of earlier output.
        markers = ["selftest: FAIL: file: " + assertion, "VOS-TEST: FAIL"]
        if label == "reserved-inode":
            markers.append("I4 metadata mutation 7 exit")
        run("env", "TIMEOUT=240", "scripts/exit-test.sh", "mutation-" + label,
            "build/x86_64-test-ext2/vos.iso", "35", *markers, "--",
            "-drive", "file=build/ext2/ext2-1k.img,if=none,id=vd0,format=raw,readonly=on",
            "-device", "virtio-blk-pci,drive=vd0,disable-legacy=on",
            cwd=target, log=output / (label + ".run.log"))
        print(label + ": detected by completed file assertions", flush=True)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main()
