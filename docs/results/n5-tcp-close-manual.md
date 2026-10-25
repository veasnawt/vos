# Manual QEMU testing of the N5 TCP closure build

These instructions reuse the already verified images and the recorded isolated
host peer. They do not rebuild, rerun the sanitizer/performance suites, modify an
ISO or write new test logs, captures, disk images or firmware variables.
Run them after automated VOS execution has ended, with no other VOS executor.
Keep the existing storage limits/reserves: do not use this as permission for
another full build/evidence attempt or remove old files to make space.

The closure image is a kernel-controlled diagnostic demo. It does not boot vsh
or provide a socket command. It needs the controlled peer below; an ordinary
standalone ISO boot with QEMU's default NAT is not this test.

## Verify your local checkpoint

In Ubuntu/WSL:

```sh
cd ~/src/myos
git branch --show-current
git rev-parse HEAD
git diff --cached --name-only
git status --short
df -h / /mnt/c /mnt/e
```

Expected branch is `feat/n5-networking`, HEAD is
`d512db7cdf40b5b3dbcaf9c18f21041943d1c22c`, and the index is empty.
The closure files intentionally remain uncommitted. The latest resume inventory
and physical-reserve/accounting snapshot are in
[resume-review.json](n5-tcp-close/resume-review.json) and
[resume-changed-files.txt](n5-tcp-close/resume-changed-files.txt).
The earlier [final review](n5-tcp-close/final-review.json) is its original
pre-PR-6 checkpoint and is preserved unchanged.
Stop if your disk capacity has fallen below the existing reserve requirements.
This demo makes no new evidence allocation; it keeps 128 MiB guest RAM and uses
4 MiB of RAM for private UEFI variables rather than touching a protected file.

GTK is installed in the recorded environment. A visible window also requires a
working WSLg/X display. To inspect supported backends without booting:
```sh
qemu-system-x86_64 -display help
printf 'DISPLAY=%s WAYLAND_DISPLAY=%s\n' "$DISPLAY" "$WAYLAND_DISPLAY"
```

## Run BIOS visibly with the recorded peer

Paste this entire block into the same WSL terminal. All peer packets are loaded
from the hash-verified BIOS wire transcript. QEMU receives them through an
inherited Unix socket pair; no TAP, NAT, host TCP listener or Internet route is used.
Keep the terminal open while the window runs.

```sh
VOS_FIRMWARE=bios VOS_DISPLAY=gtk PYTHONDONTWRITEBYTECODE=1 python3 -B - <<'PY'
import hashlib, json, os, resource, socket, struct, subprocess
from pathlib import Path

repo = Path.cwd()
fw = os.environ.get("VOS_FIRMWARE", "bios")
display = os.environ.get("VOS_DISPLAY", "gtk")
if fw not in ("bios", "uefi") or display not in ("gtk", "none"):
    raise SystemExit("Use bios/uefi and gtk/none.")
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
resource.setrlimit(resource.RLIMIT_FSIZE, (0, 4 * 1024**2))  # no disk outputs supplied
index = json.loads((repo / "docs/results/n5-tcp-close/evidence.json").read_text())
results = (repo / index["local_manifest"]["path"]).parent
iso_name = "vos.iso" if fw == "bios" else "vos-uefi.iso"
image_key = "native/kernel/" + iso_name
iso = results / image_key
transcript = repo / ("docs/results/n5-tcp-close/" + fw + "-wire.json")

def digest(p):
    with p.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()

if digest(iso) != index["products"][image_key]:
    raise SystemExit("STOP: verified ISO identity changed.")
expected = next(a["sha256"] for a in index["artifacts"]
                if a["path"] == str(transcript.relative_to(repo)))
if digest(transcript) != expected:
    raise SystemExit("STOP: peer transcript identity changed.")
records = json.loads(transcript.read_text())
if len(records) != 49 or sum(r["direction"] == "TX" for r in records) != 23:
    raise SystemExit("STOP: unexpected peer transcript.")

peer, guest = socket.socketpair()
peer.settimeout(45)
pass_fds = [guest.fileno()]
args = [
    "qemu-system-x86_64", "-machine", "q35", "-accel", "tcg", "-m", "128M",
    "-cdrom", str(iso), "-boot", "d", "-display", display, "-nic", "none",
    "-serial", "stdio", "-no-reboot", "-no-shutdown",
    "-netdev", "stream,id=n0,server=off,addr.type=fd,addr.str=" + str(guest.fileno()),
    "-device", "virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56",
]
nvram_fd = None
if fw == "uefi":
    # Private anonymous RAM file: the packaged OVMF variables stay unchanged.
    nvram_fd = os.memfd_create("vos-manual-ovmf", os.MFD_CLOEXEC)
    variables = Path("/usr/share/OVMF/OVMF_VARS_4M.fd").read_bytes()
    # F_SIZE=0 would also prohibit writes to memfd, so allow this one fixed
    # RAM-backed size. No writable disk device or output file is supplied.
    resource.setrlimit(resource.RLIMIT_FSIZE, (4 * 1024**2, 4 * 1024**2))
    os.write(nvram_fd, variables)
    pass_fds.append(nvram_fd)
    args += [
        "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
        "-drive", "if=pflash,format=raw,unit=1,file=/proc/self/fd/" + str(nvram_fd),
    ]

def read_exact(n):
    data = bytearray()
    while len(data) < n:
        part = peer.recv(n - len(data))
        if not part:
            raise RuntimeError("QEMU peer closed before the expected exchange.")
        data.extend(part)
    return bytes(data)

p = None
try:
    p = subprocess.Popen(args, pass_fds=tuple(pass_fds))
    guest.close()
    if nvram_fd is not None:
        os.close(nvram_fd)
        nvram_fd = None
    for r in records:
        raw = bytes.fromhex(r["hex"])
        if not 14 <= len(raw) <= 1514:
            raise RuntimeError("Invalid recorded frame bounds.")
        if r["direction"] == "RX":
            peer.sendall(struct.pack("!I", len(raw)) + raw)
        elif r["direction"] == "TX":
            n = struct.unpack("!I", read_exact(4))[0]
            if not 14 <= n <= 1514 or read_exact(n) != raw:
                raise RuntimeError("Guest frame differs from the verified transcript: " + r["label"])
        else:
            raise RuntimeError("Unknown transcript direction.")
    print("\nPeer replay complete. Expect the guest PASS line after FIN timeout.", flush=True)
    print("Leave the window open to inspect VOS; close QEMU or press Ctrl+C to exit.", flush=True)
    peer.settimeout(0.25)
    while p.poll() is None:
        try:
            extra = peer.recv(1518)
            if extra:
                raise RuntimeError("Unexpected packet after the verified exchange.")
            if extra == b"":
                p.wait(timeout=3)
                break
        except socket.timeout:
            pass
except KeyboardInterrupt:
    pass
finally:
    peer.close()
    guest.close()
    if nvram_fd is not None:
        os.close(nvram_fd)
    if p is not None and p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=3)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
PY
```

This is an owner-driven manual replay, not a replacement for the automated
Codex controller or a new accepted verification campaign. The existing guarded
BIOS/UEFI evidence remains authoritative. The visible-display and anonymous-RAM
UEFI-variable invocation is documented for you to exercise; it was not used to
claim an additional passing runtime result.

## UEFI or serial-only

To use UEFI, run the same block with its first line changed to:

```sh
VOS_FIRMWARE=uefi VOS_DISPLAY=gtk PYTHONDONTWRITEBYTECODE=1 python3 -B - <<'PY'
```

Keep the Python body and final `PY` delimiter identical.
If WSLg is unavailable, change `VOS_DISPLAY=gtk` to
`VOS_DISPLAY=none`. The real VOS serial console appears in the terminal.
Ctrl+C exits the replay and terminates only its own QEMU child.

Expected final guest output:

```text
tcp-close: PASS TX=23 RX=26; FIN/reset/timeout cleanup; DMA frames released=11
Kernel-only; bounded diagnostic TCP, not production-ready
```

The guest also reports retained-data recovery after peer FIN, recovery from a
lost FIN ACK, active/simultaneous TIME-WAIT, rejection of invalid controls,
exact-sequence reset erasure and three FIN retries followed by terminal timeout.
The active linger is explicitly locally aborted; a 240-second live linger
completion is not claimed.

## Inspect existing accepted artifacts without another boot

```sh
cat docs/results/n5-tcp-close/bios.serial.txt
cat docs/results/n5-tcp-close/uefi.serial.txt
sha256sum docs/results/n5-tcp-close/{bios,uefi}.pcap
explorer.exe "$(wslpath -w "$PWD/docs/images/n5/vos-n5-bios-tcp-close.png")"
```

The full build/sanitizer/performance workflow remains in
[the technical report](n5-tcp-close.md). Do not run it under the assumption that
the original attempt reservation is unused: all failed and successful attempts
remain charged. Any fresh artifact-producing attempt requires new admission.
