#!/usr/bin/env python3
"""Fail-closed Gate A host supervision. No cleanup or implicit test selection."""
import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import time

MIB = 1024 ** 2
GIB = 1024 ** 3
LIMIT = 384 * MIB


def require(ok, why):
    if not ok:
        raise RuntimeError(why)


def allocated(path):
    """Blocks allocated under path now, never following a symlink. An entry that no longer
    exists, including one removed while it is measured (ENOENT: a tool's temporary file or
    directory), holds no blocks and counts 0; every other error propagates (fail closed)."""
    p = Path(path)
    try:
        s = p.lstat()
        children = list(p.iterdir()) if stat.S_ISDIR(s.st_mode) else []
    except FileNotFoundError:
        return 0
    return s.st_blocks * 512 + sum(allocated(x) for x in children)


def host_free():
    command = ("$ErrorActionPreference='Stop'; "
               "$d=@(Get-ChildItem HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\Lxss | "
               "Get-ItemProperty | Where-Object DistributionName -eq 'Ubuntu'); "
               "if($d.Count -ne 1){throw 'Ubuntu registration not unique'}; "
               "[pscustomobject]@{base=[string]$d[0].BasePath;"
               "C=[int64](Get-PSDrive C).Free;E=[int64](Get-PSDrive E).Free}|ConvertTo-Json -Compress")
    p = subprocess.run(['/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe',
                        '-NoProfile', '-NonInteractive', '-Command', command],
                       capture_output=True, check=True, timeout=15)
    data = json.loads(p.stdout.decode('utf-8-sig'))
    require(data['base'].upper().lstrip('\\?').startswith('C:'), 'Ubuntu backing volume unknown')
    require(type(data['C']) is int and type(data['E']) is int, 'invalid host measurements')
    return data


class Supervisor:
    def __init__(self, root):
        self.root = Path(root).resolve(strict=True)
        self.state = json.loads((self.root / 'state.json').read_text())
        require(self.state['limit_bytes'] == LIMIT, 'unexpected phase allowance')
        self.archive = Path(self.state['evidence_copy'])

    def guard(self, extra=0):
        require(type(extra) is int and extra >= 0, 'invalid operation admission')
        used = allocated(self.root) + allocated(self.archive)
        charged = used + 2 * MIB  # directory/record headroom
        require(charged + extra <= LIMIT, 'Gate A aggregate allowance exhausted')
        require(allocated(self.root / '.tmp') <= 64 * MIB, 'temporary allowance exhausted')
        remaining = LIMIT - charged
        host = host_free()
        require(host['C'] >= 10 * GIB + remaining + 256 * MIB, 'C reserve insufficient')
        require(host['E'] >= extra + 256 * MIB, 'E reserve insufficient')
        fs = os.statvfs(self.root)
        free = fs.f_bavail * fs.f_frsize
        require(free >= GIB + remaining, 'Linux reserve insufficient')
        row = dict(utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                   allocated=used, charged=charged, admitted_extra=extra,
                   C_free=host['C'], E_free=host['E'], linux_free=free)
        with (self.root / 'storage-samples.jsonl').open('a') as f:
            f.write(json.dumps(row) + '\n')
        return row

    def run(self, label, argv, *, extra, cwd=None, env_extra=None, timeout=300,
            expected=0, file_limit=64 * MIB):
        require(label and '/' not in label and label not in ('.', '..'), 'invalid label')
        self.guard(extra)
        log = self.root / 'logs' / (label + '.log')
        require(not log.exists(), 'refusing to overwrite evidence')
        env = os.environ.copy()
        for key in ['MAKEFLAGS', 'MFLAGS', 'MAKEFILES', 'GNUMAKEFLAGS', 'MAKELEVEL', 'CC', 'CFLAGS', 'LDFLAGS']:
            env.pop(key, None)
        env.update(GIT_OPTIONAL_LOCKS='0', PYTHONDONTWRITEBYTECODE='1',
                   TMPDIR=str(self.root / '.tmp'), TMP=str(self.root / '.tmp'),
                   TEMP=str(self.root / '.tmp'))
        env.update(env_extra or {})
        def limits():
            resource.setrlimit(resource.RLIMIT_FSIZE, (file_limit, file_limit))
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        row = dict(label=label, argv=list(map(str, argv)), cwd=str(cwd or self.root),
                   env_extra=env_extra or {}, expected=expected, timeout=timeout,
                   worst_additional=extra, log=str(log.relative_to(self.root)))
        start = time.monotonic()
        proc = None
        error = None
        try:
            with log.open('x') as out:
                proc = subprocess.Popen(row['argv'], cwd=cwd or self.root, env=env,
                                        stdout=out, stderr=subprocess.STDOUT,
                                        start_new_session=True, preexec_fn=limits)
                while proc.poll() is None:
                    self.guard()
                    require(time.monotonic() - start < timeout, 'host watchdog expired')
                    time.sleep(.5)  # capacity monitoring, never a guest assertion
                self.guard()
                require(proc.returncode == expected, 'unexpected child exit: ' + str(proc.returncode))
        except BaseException as exc:
            error = str(exc)
            raise
        finally:
            if proc:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
            row.update(returncode=proc.returncode if proc else None,
                       seconds=time.monotonic() - start, error=error)
            with (self.root / 'commands.jsonl').open('a') as f:
                f.write(json.dumps(row) + '\n')
        print(json.dumps(row), flush=True)
        return log

    def copy(self, source, target):
        source, target = Path(source), Path(target)
        require(not os.path.lexists(target), 'copy target already exists')
        s = source.lstat()
        require(stat.S_ISREG(s.st_mode) and s.st_size <= 64 * MIB, 'copy not allowlisted regular input')
        self.guard((s.st_size + 4095) // 4096 * 4096 + 4096)
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '--sparse=always', '--reflink=never', '--no-dereference',
                        '--preserve=mode,timestamps', '--', str(source), str(target)], check=True)
        require(target.stat().st_size == s.st_size and
                hashlib.sha256(target.read_bytes()).digest() == hashlib.sha256(source.read_bytes()).digest(),
                'copy content mismatch')
        if s.st_size > 4096 and s.st_blocks * 512 < s.st_size:
            require(target.stat().st_blocks * 512 < s.st_size, 'sparse input expanded')
        self.guard()
