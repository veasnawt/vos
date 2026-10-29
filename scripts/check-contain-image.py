#!/usr/bin/env python3
"""USB.2-C3b.2 image-flavour check: a kernel is either production (no controller-
containment executor of any kind) or the experimental QEMU-only containment test
image (identified by an unmistakable marker). Derived from the linked binary, not
from build variables or directory names, so a smuggled compiler flag, a stale
object or a replaced kernel cannot slip an executor into a production image.

  production: none of the executor symbols, the execute option string or the marker;
  test:       the marker, the four executor symbols and the option string, all present.
usage: check-contain-image.py --expect production|test KERNEL...        (exit 0 if every kernel passes)
       check-contain-image.py --expect production --iso ISO...          (the /boot/vos.bin inside, plus a raw scan of
                                                                          the whole image for the marker and option)
"""
import os
import re
import subprocess
import sys
import tempfile

MARKER = b'VOS-CONTAIN-TEST-IMAGE: QEMU-ONLY EXPERIMENTAL; NEVER FOR PHYSICAL PCS'
OPTION = b'vos.usb2-contain-execute=1'
SYMBOLS = ('xhci_contain_w1', 'xhci_contain_w2', 'vos_xhci_contain_execute_v1', 'xhci_contain_execute_boot',
           'vos_contain_test_image')


def check(path, expect):
    data = open(path, 'rb').read()
    names = {l.split()[-1] for l in subprocess.check_output(['nm', path], text=True).splitlines() if len(l.split()) == 3}
    mangled = subprocess.check_output(['nm', path], text=True)
    found = {
        'marker': MARKER in data,
        'option': OPTION in data,
        'symbols': sorted(s for s in SYMBOLS if s in names),
        'rust_executor': bool(re.search(r'xhci_contain\w*execute|containment\w*stages|10Effects', mangled)),
    }
    want_test = expect == 'test'
    ok = (found['marker'] == want_test and found['option'] == want_test and
          (len(found['symbols']) == len(SYMBOLS)) == want_test and (not found['symbols'] or want_test) and
          found['rust_executor'] == want_test)
    return ok, found


def check_iso(path, expect):
    """The kernel inside an ISO must be of the expected flavour, and a production image must contain no trace of
    the executor anywhere (bootloader configuration included)."""
    with tempfile.TemporaryDirectory() as tmp:
        kernel = os.path.join(tmp, 'vos.bin')
        subprocess.run(['xorriso', '-osirrox', 'on', '-indev', path, '-extract', '/boot/vos.bin', kernel],
                       check=True, capture_output=True)
        ok, found = check(kernel, expect)
    raw = open(path, 'rb').read()
    found['iso_raw_marker'] = MARKER in raw
    found['iso_raw_option'] = OPTION in raw
    if expect == 'production' and (found['iso_raw_marker'] or found['iso_raw_option']):
        ok = False
    return ok, found


def main(argv):
    iso = len(argv) > 3 and argv[3] == '--iso'
    if len(argv) < 4 + iso or argv[1] != '--expect' or argv[2] not in ('production', 'test'):
        sys.exit(__doc__)
    bad = 0
    for path in argv[3 + iso:]:
        ok, found = (check_iso if iso else check)(path, argv[2])
        if not ok:
            bad += 1
            print(f'{path}: NOT a {argv[2]} image: {found}', file=sys.stderr)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main(sys.argv)
