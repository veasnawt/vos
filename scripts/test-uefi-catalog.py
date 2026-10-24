#!/usr/bin/env python3
"""Keep all 13 accepted case inputs/expectations; add complete-row boundaries."""
import argparse
import json
from pathlib import Path
import sys
from uefi_catalog import parse_catalog, parse_extent, valid_catalog

p = argparse.ArgumentParser()
p.add_argument("original_cases", type=Path)
p.add_argument("output", type=Path)
args = p.parse_args()
original = json.loads(args.original_cases.read_text())["cases"]
assert len(original) == 13
results = []
for case in original:
    actual = valid_catalog(case["input"])
    results.append(dict(name=case["name"], group="original13",
                        expected=case["expected"], actual=actual,
                        passed=actual == case["expected"]))
valid = original[0]["input"]
row = next(x for x in valid.split("\n") if x.startswith("El Torito boot img :"))
path = next(x for x in valid.split("\n") if x.startswith("El Torito img path :"))
def changed(old, new):
    assert old in row
    return valid.replace(row, row.replace(old, new))
extra = [
    ("duplicate-row", valid + "\n" + row, False),
    ("contradictory-row", valid + "\n" + row.replace("UEFI", "BIOS"), False),
    ("duplicate-image-path", valid + "\n" + path, False),
    ("wrong-image-path", valid.replace(path, path.replace("/efi.img", "/other.img")), False),
    ("missing-image-path", valid.replace(path, ""), False),
    ("extra-trailing-field", valid.replace(row, row + " garbage"), False),
    ("not-bootable", changed("UEFI  y", "UEFI  n"), False),
    ("wrong-emulation", changed("none", "hd"), False),
    ("bad-load-segment", changed("0x0000", "0x000g"), False),
    ("unsupported-load-segment", changed("0x0000", "0x1000"), False),
    ("bad-partition-type", changed("0x00 ", "0xgg "), False),
    ("zero-sectors", changed("5760", "0"), False),
    ("negative-sectors", changed("5760", "-1"), False),
    ("oversized-sectors", changed("5760", "65536"), False),
    ("zero-LBA", changed("54", "0"), False),
    ("negative-LBA", changed("54", "-1"), False),
    ("non-ASCII-separator", valid.replace(row, row.replace("UEFI  y", "UEFI\u00a0y")), False),
    ("vertical-tab", valid.replace(row, row.replace("UEFI  y", "UEFI\vy")), False),
    ("embedded-CR", valid.replace(row, row.replace("UEFI  y", "UEFI\ry")), False),
    ("leading-indentation", valid.replace(row, "\t  " + row), True),
    ("horizontal-tabs", valid.replace(row, row.replace("  ", "\t")), True),
    ("CRLF-lines", valid.replace("\n", "\r\n"), True),
    ("trailing-horizontal-space", valid.replace(row, row + " \t"), True),
]
for name, text, expected in extra:
    actual = valid_catalog(text)
    results.append(dict(name=name, group="extra", expected=expected,
                        actual=actual, passed=actual == expected))
# Location and size are parsed fields, not constants for subsequent builds.
relocated = valid.replace(row, row.replace("5760", "4096").rsplit("54", 1)[0] + "100")
location = parse_catalog(relocated)
results.append(dict(name="non-hardcoded-location", group="extra",
                    expected=True, actual=location["lba2048"] == 100 and location["sectors512"] == 4096,
                    passed=location["lba2048"] == 100 and location["sectors512"] == 4096))
failed = [x for x in results if not x["passed"]]
report = dict(original_passed=sum(x["passed"] for x in results if x["group"] == "original13"),
              original_total=13, extra_total=len(results)-13, failed=failed, cases=results)
with args.output.open("x") as f:
    f.write(json.dumps(report, indent=2) + "\n")
for x in results:
    print(("PASS " if x["passed"] else "FAIL ") + x["group"] + " " + x["name"])
print("TOTAL",len(results),"FAILED",len(failed))
sys.exit(1 if failed else 0)
