#!/usr/bin/env python3
"""Negative controls for N3.5: mutate fresh scratch copies, never the source tree.

Usage: python3 scripts/test-file-mutations.py SOURCE VERIFIED_BUILD OUTPUT
       [--control LABEL ...]
Omitting --control retains all five controls, in their original order.
SOURCE is the Git worktree; VERIFIED_BUILD is its matching, built scratch tree.
OUTPUT must not exist. Needs the normal BIOS test dependencies. Each control
must finish with a concrete file assertion failure and the final FAIL verdict;
a panic, timeout, compiler error or a lost PASS marker is not detection.
"""
import argparse
import json
import pathlib
import os
import resource
import shutil
import signal
import stat
import subprocess
import tarfile
import time

MIB = 1024 ** 2
GIB = 1024 ** 3
TOTAL_LIMIT = GIB
WORKSPACE_LIMIT = 128 * MIB
TEMP_LIMIT = 64 * MIB
FILE_LIMIT = 64 * MIB
SOURCE_LIMIT = 16 * MIB
HOST_RESERVE = 10 * GIB
LINUX_RESERVE = GIB

# Deliberately exclude fixture source trees, other images, ISOs and old logs.
KERNEL_OBJECTS = """
boot entry usermode kernel bootinfo console cpu exception gdt idt lib loader
panic pmm process resv syscall usercopy vmm kmap irq pic pit thread thread_switch
channel elf app pci blockdev virtio_blk ext2 io_scope file devmap screen font8x16
selftest selftest_cpu selftest_elf selftest_ipc selftest_irq selftest_kmap
selftest_mp selftest_sched selftest_stress selftest_pmm selftest_suite
selftest_user selftest_vblk selftest_blk selftest_ext2 selftest_vmm selftest_screen
selftest_io_scope selftest_file storage_observe user_progs
""".split()
RUNTIME_OBJECTS = "crt0 syscall syscall_v1 syscall_v2 abi_note abi_note_v1 abi_note_v2".split()
FILE_PROGRAMS = "probe reader reader_y error gate0 gate1 boundary nogrant cookie regs adversary".split()


def artifact_paths():
    paths = [pathlib.Path("build/x86_64-test-file") / (n + suffix)
             for n in KERNEL_OBJECTS for suffix in (".o", ".d")]
    paths += [pathlib.Path("build/x86_64/user/lib") / (n + suffix)
              for n in RUNTIME_OBJECTS for suffix in (".o", ".d")]
    paths += [pathlib.Path("build/x86_64/user/filetests") / (n + suffix)
              for n in FILE_PROGRAMS for suffix in (".o", ".d", ".elf")]
    paths += [pathlib.Path(n) for n in (
        "build/x86_64/user/hello/hello.o", "build/x86_64/user/hello/hello.d",
        "build/x86_64/user/hello.elf", "build/ext2-tools/fixgen",
        "build/ext2/ext2-1k.img", "build/ext2/SHA256SUMS")]
    return sorted(paths)


def validate_inputs(verified):
    paths = artifact_paths()
    for name in paths:
        st = (verified / name).lstat()  # missing, links and specials fail closed
        if not stat.S_ISREG(st.st_mode) or st.st_size > FILE_LIMIT:
            raise RuntimeError("invalid or oversized build artifact: " + str(name))
    stamp = verified / "build/ext2/SHA256SUMS"
    generator = verified / "build/ext2-tools/fixgen"
    if generator.stat().st_mtime_ns < (verified / "scripts/fixgen.c").stat().st_mtime_ns:
        raise RuntimeError("fixture generator would rebuild")
    if any(stamp.stat().st_mtime_ns < p.stat().st_mtime_ns for p in
           (generator, verified / "scripts/mkext2fixtures.sh")):
        raise RuntimeError("fixtures would regenerate")
    if sum((verified / p).stat().st_size for p in paths) > WORKSPACE_LIMIT // 2:
        raise RuntimeError("allowlisted inputs exceed admission budget")
    return paths


def allocated(root):
    # scandir + lstat avoids even resolving a link to classify its target.
    # os.walk classifies links via is_dir(), which can advance link atime.
    try:
        st = os.lstat(root)
    except FileNotFoundError:
        return 0
    total = st.st_blocks * 512
    if stat.S_ISDIR(st.st_mode):
        with os.scandir(root) as entries:
            for entry in entries:
                total += allocated(entry.path)
    return total


def host_free():
    # Query the physical backing volume, not WSL's much larger virtual df.
    # Refuse a moved/different distribution rather than guessing its host disk.
    command = (
        "$ErrorActionPreference='Stop'; "
        r"$d=@(Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss | "
        "Get-ItemProperty | Where-Object DistributionName -eq 'Ubuntu'); "
        "if($d.Count -ne 1 -or $d[0].BasePath -notmatch '^C:')"
        "{throw 'Ubuntu backing volume is not confirmed C:'}; "
        "[pscustomobject]@{volume='C:';free=[int64](Get-PSDrive C).Free}"
        "|ConvertTo-Json -Compress"
    )
    result = subprocess.check_output(
        ["/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
         "-NoProfile", "-NonInteractive", "-Command", command], timeout=15)
    data = json.loads(result.decode("utf-8-sig"))
    if data.get("volume") != "C:" or type(data.get("free")) is not int or data["free"] < 0:
        raise RuntimeError("invalid Windows host-space measurement")
    return data["free"]


class StorageGuard:
    def __init__(self, root):
        self.root = pathlib.Path(root)
        self.workspace = None
        self.last_host_check = 0
        self.host_available = None

    def check(self, reserve=0, force=False):
        if not 0 <= reserve <= WORKSPACE_LIMIT:
            raise RuntimeError("invalid admission reservation")
        if force or time.monotonic() - self.last_host_check >= 1:
            self.host_available = host_free()  # exceptions deliberately propagate
            self.last_host_check = time.monotonic()
        if self.host_available < HOST_RESERVE + reserve:
            raise RuntimeError("Windows C: free-space reserve would be violated")
        probe = self.root
        while not probe.exists():
            probe = probe.parent
        if shutil.disk_usage(probe).free < LINUX_RESERVE + reserve:
            raise RuntimeError("Linux free-space reserve would be violated")
        if allocated(self.root) + reserve > TOTAL_LIMIT:
            raise RuntimeError("phase allocation budget exceeded")
        if self.workspace is not None:
            if allocated(self.workspace) + reserve > WORKSPACE_LIMIT:
                raise RuntimeError("workspace allocation budget exceeded")
            if allocated(self.workspace / ".tmp") > TEMP_LIMIT:
                raise RuntimeError("temporary-output allocation budget exceeded")


def child_limits():
    resource.setrlimit(resource.RLIMIT_FSIZE, (FILE_LIMIT, FILE_LIMIT))


def run(*args, cwd, log, guard):
    guard.check(force=True)
    env = os.environ.copy()
    # Do not inherit make flags that could enable recursion or unexpected goals.
    for name in ("MAKEFLAGS", "MFLAGS", "MAKEFILES", "GNUMAKEFLAGS", "MAKELEVEL"):
        env.pop(name, None)
    temp = pathlib.Path(cwd) / ".tmp"
    temp.mkdir(exist_ok=True)
    env.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp))
    with log.open("x") as stream:
        proc = subprocess.Popen(args, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                env=env, start_new_session=True, preexec_fn=child_limits)
        deadline = time.monotonic() + 900
        try:
            while proc.poll() is None:
                guard.check()
                if time.monotonic() >= deadline:
                    raise RuntimeError("bounded harness subprocess deadline exceeded")
                time.sleep(0.5)
            guard.check(force=True)
            if proc.returncode:
                raise subprocess.CalledProcessError(proc.returncode, args)
        finally:
            # Retire descendants too, on errors or guard trips. Preserve outputs.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()


def copy_artifact(source, destination, guard):
    source, destination = pathlib.Path(source), pathlib.Path(destination)
    st = source.lstat()
    if destination.exists() or destination.is_symlink():
        raise RuntimeError("refusing to overwrite artifact: " + str(destination))
    guard.check(reserve=st.st_size if stat.S_ISREG(st.st_mode) else 4096)
    if stat.S_ISREG(st.st_mode):
        if st.st_size > FILE_LIMIT:
            raise RuntimeError("oversized copy input")
        # No hard links, reflinks, sparse autodetection, or dense fallback.
        subprocess.run(["cp", "--sparse=always", "--reflink=never",
                        "--preserve=mode,timestamps,xattr", "--", str(source), str(destination)],
                       check=True, preexec_fn=child_limits, timeout=30)
        copied = destination.lstat()
        if copied.st_size != st.st_size:
            raise RuntimeError("copy changed logical size")
        if st.st_blocks * 512 < st.st_size and copied.st_blocks * 512 >= st.st_size:
            raise RuntimeError("sparse allocation lost; refusing dense copy")
        if st.st_blocks == 0 and copied.st_blocks != 0:
            raise RuntimeError("all-hole copy allocated data blocks")
    elif stat.S_ISLNK(st.st_mode):
        os.symlink(os.readlink(source), destination)
        shutil.copystat(source, destination, follow_symlinks=False)
        # readlink can advance the source link's atime before copystat.
        os.utime(destination, ns=(st.st_atime_ns, st.st_mtime_ns), follow_symlinks=False)
    elif stat.S_ISFIFO(st.st_mode):
        os.mkfifo(destination, stat.S_IMODE(st.st_mode))
        shutil.copystat(source, destination, follow_symlinks=False)
    else:
        raise RuntimeError("unsupported artifact type")
    guard.check()
    return destination


def copy_build(verified, target, guard):
    paths = validate_inputs(verified)
    directories = set()
    for name in paths:
        destination = target / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        copy_artifact(verified / name, destination, guard)
        directories.update(p for p in name.parents if p != pathlib.Path("."))
    # Preserve directory times only after all children have been created.
    for name in sorted(directories, key=lambda p: len(p.parts), reverse=True):
        shutil.copystat(verified / name, target / name, follow_symlinks=False)
    guard.check(force=True)


def dependency_plan(target, changed, guard, log):
    # -n does not execute target recipes. Pin included .d files against remake;
    # this audited Makefile has no recursive recipes on the iso dependency path.
    pinned = [arg for p in artifact_paths() if p.suffix == ".d"
              for arg in ("-o", str(p))]
    run("make", "-n", "-j1", *pinned, "-W", changed,
        "ARCH=x86_64", "VOS_TEST=1", "VOS_TEST_CASE=file", "iso",
        cwd=target, log=log, guard=guard)
    lines = log.read_text().splitlines()
    compiles = [line for line in lines if line.startswith("gcc ")]
    if len(compiles) != 1 or ("-c " + changed + " -o ") not in compiles[0]:
        raise RuntimeError("unexpected rebuild in dependency plan; inspect " + str(log))
    if any(token in line for line in lines for token in
           ("mkext2fixtures", "fixgen.c", "mke2fs", "e2fsck", "make ", "qemu")):
        raise RuntimeError("unexpected fixture/test/recursive dependency")
    if not any(line.startswith("grub-mkrescue ") for line in lines):
        raise RuntimeError("missing ISO build dependency")


def select_controls(controls, requested):
    if len(set(requested)) != len(requested) or set(requested) - controls.keys():
        raise ValueError("unknown or duplicate control")
    return requested or list(controls)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("verified", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--control", action="append", default=[])
    args = parser.parse_args()
    source, verified, output = (p.resolve() for p in (args.source, args.verified, args.output))
    guard = StorageGuard(output)
    guard.check(reserve=WORKSPACE_LIMIT, force=True)
    for tool in ("cp", "make", "gcc", "ld", "grub-file", "grub-mkrescue",
                 "xorriso", "qemu-system-x86_64", "timeout"):
        if not shutil.which(tool):
            raise RuntimeError("required dependency missing: " + tool)
    validate_inputs(verified)
    if output.exists():
        raise RuntimeError("output must not exist")
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
    try:
        selected = select_controls(controls, args.control)
    except ValueError as error:
        parser.error(str(error))
    # Bound source/archive size before writing; never archive ignored fixture trees.
    if sum((source / name).lstat().st_size for name in set(names)) > SOURCE_LIMIT // 2:
        raise RuntimeError("source archive exceeds admission budget")
    output.mkdir()  # refuse an existing target
    archive = output / "source.tar"
    with tarfile.open(archive, "w") as tar:
        for name in sorted(set(names)):
            tar.add(source / name, arcname=name, recursive=False)
    if archive.stat().st_size > SOURCE_LIMIT:
        raise RuntimeError("source archive exceeds storage budget")
    guard.check(force=True)
    for label in selected:
        name, original, replacement, assertion = controls[label]
        guard.workspace = None
        guard.check(reserve=WORKSPACE_LIMIT, force=True)
        target = output / label
        target.mkdir()
        guard.workspace = target
        (target / ".tmp").mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(target, filter="data")
        guard.check()
        copy_build(verified, target, guard)
        dependency_plan(target, name, guard, output / (label + ".plan.log"))
        path = target / name
        text = path.read_text()
        assert text.count(original) == 1, label
        path.write_text(text.replace(original, replacement))
        run("make", "-j1", "ARCH=x86_64", "VOS_TEST=1", "VOS_TEST_CASE=file", "iso",
            cwd=target, log=output / (label + ".build.log"), guard=guard)
        # This runner checks guest exit 35, both kernel-only markers and no
        # triple fault. A timeout returns failure regardless of earlier output.
        markers = ["selftest: FAIL: file: " + assertion, "VOS-TEST: FAIL"]
        if label == "reserved-inode":
            markers.append("I4 metadata mutation 7 exit")
        run("env", "TIMEOUT=240", "scripts/exit-test.sh", "mutation-" + label,
            "build/x86_64-test-file/vos.iso", "35", *markers, "--",
            "-drive", "file=build/ext2/ext2-1k.img,if=none,id=vd0,format=raw,readonly=on",
            "-device", "virtio-blk-pci,drive=vd0,disable-legacy=on",
            cwd=target, log=output / (label + ".run.log"), guard=guard)
        print(label + ": detected by completed file assertions", flush=True)


if __name__ == "__main__":
    main()
