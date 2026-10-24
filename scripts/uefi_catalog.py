#!/usr/bin/env python3
"""Strict xorriso plain-report validation for the Gate A UEFI-only ISO.

Columns follow xorriso -report_el_torito help. This profile requires exactly
one bootable, non-emulated UEFI entry numbered 1 and its named EFI image.
LBA and sector count are parsed, never assumed to be fixture constants.
"""
import hashlib
import json
from pathlib import Path
import re
import struct


def need(ok, message):
    if not ok:
        raise ValueError(message)


def decimal(token, maximum):
    need(re.fullmatch(r"0|[1-9][0-9]*", token) is not None, "invalid decimal field")
    value = int(token)
    need(value <= maximum, "decimal field out of range")
    return value


def physical_lines(text):
    for raw in text.split("\n"):
        if raw.endswith("\r"):
            raw = raw[:-1]  # CRLF is one physical line ending.
        yield raw


def record(line, prefix):
    line = line.strip(" \t")
    need(not any(ord(c) < 32 and c != "\t" for c in line), "control in record")
    left, separator, right = line.partition(":")
    need(separator == ":" and left.rstrip(" \t") == prefix, "malformed record prefix")
    need(right.startswith((" ", "\t")), "missing field separator")
    return re.split(r"[ \t]+", right.strip(" \t"))


def parse_catalog(text, expected_path="/efi.img"):
    rows, paths = [], []
    for raw in physical_lines(text):
        line = raw.lstrip(" \t")
        if line.startswith("El Torito boot img"):
            fields = record(line, "El Torito boot img")
            need(len(fields) == 8, "boot row must have all eight fields")
            number, platform, bootable, emulation, segment, partition, sectors, lba = fields
            need(number == "1" and platform == "UEFI", "unexpected entry/platform")
            need(bootable == "y" and emulation == "none", "UEFI entry not bootable/non-emulated")
            need(re.fullmatch(r"0x[0-9a-fA-F]{4}", segment) is not None, "invalid load segment")
            need(re.fullmatch(r"0x[0-9a-fA-F]{2}", partition) is not None, "invalid partition type")
            need(int(segment, 16) == 0 and int(partition, 16) == 0, "unsupported Gate A boot fields")
            sectors = decimal(sectors, 65535)
            lba = decimal(lba, (1 << 32) - 1)
            need(sectors > 0 and lba > 0, "empty or zero-location image")
            rows.append(dict(entry=1, platform=platform, bootable=bootable,
                             emulation=emulation, load_segment=0, partition_type=0,
                             sectors512=sectors, lba2048=lba))
        elif line.startswith("El Torito img path"):
            fields = record(line, "El Torito img path")
            need(len(fields) == 2 and fields[0] == "1", "malformed image association")
            need(fields[1] == expected_path, "unexpected EFI image path")
            paths.append(fields[1])
    need(len(rows) == 1, "missing, duplicate or contradictory boot rows")
    need(len(paths) == 1, "missing, duplicate or contradictory image association")
    return dict(rows[0], image_path=paths[0])


def valid_catalog(text):
    try:
        parse_catalog(text)
        return True
    except ValueError:
        return False


def parse_extent(text, expected_path):
    extents = []
    for raw in physical_lines(text):
        if not raw.lstrip(" \t").startswith("File data lba:"):
            continue
        line = raw.strip(" \t")
        need(not any(ord(c) < 32 and c != "\t" for c in line), "control in extent")
        fields = line[len("File data lba:"):].split(",", 4)
        need(len(fields) == 5, "incomplete extent")
        index, lba, blocks, size = [decimal(x.strip(" \t"), (1 << 63) - 1) for x in fields[:4]]
        need(index == 0 and fields[4].strip(" \t") == "'" + expected_path + "'", "unexpected extent path/index")
        need(size > 0 and blocks == (size + 2047) // 2048, "invalid extent size")
        extents.append(dict(lba=lba, blocks=blocks, size=size))
    need(len(extents) == 1, "missing, duplicate or fragmented EFI extent")
    return extents[0]


def validate_fat(data):
    need(len(data) >= 512 and data[510:512] == b"\x55\xaa", "invalid FAT boot signature")
    need(data[0] in (0xEB, 0xE9), "invalid FAT jump")
    sector = struct.unpack_from("<H", data, 11)[0]
    cluster = data[13]
    reserved = struct.unpack_from("<H", data, 14)[0]
    fats = data[16]
    roots = struct.unpack_from("<H", data, 17)[0]
    total = struct.unpack_from("<H", data, 19)[0] or struct.unpack_from("<I", data, 32)[0]
    fat_sectors = struct.unpack_from("<H", data, 22)[0]
    need(sector == 512 and cluster and cluster <= 128 and not cluster & (cluster - 1), "invalid FAT geometry")
    need(reserved > 0 and fats in (1, 2) and total * sector == len(data), "FAT volume size mismatch")
    if not fat_sectors:  # FAT32 BPB
        fat_sectors = struct.unpack_from("<I", data, 36)[0]
    root_sectors = (roots * 32 + sector - 1) // sector
    need(fat_sectors > 0 and reserved + fats * fat_sectors + root_sectors < total, "FAT metadata outside volume")
    return dict(bytes_per_sector=sector, sectors_per_cluster=cluster, sectors=total)


def validate_pe(data):
    need(len(data) >= 64 and data[:2] == b"MZ", "EFI loader has no DOS header")
    pe = struct.unpack_from("<I", data, 60)[0]
    need(pe + 24 + 70 <= len(data) and data[pe:pe + 4] == b"PE\0\0", "invalid EFI PE header")
    need(struct.unpack_from("<H", data, pe + 4)[0] == 0x8664, "EFI loader is not x86_64")
    optional = pe + 24
    need(struct.unpack_from("<H", data, optional)[0] == 0x20B, "EFI loader is not PE32+")
    need(struct.unpack_from("<H", data, optional + 68)[0] == 10, "loader is not an EFI application")


def validate_iso(iso, output, supervisor, env=None):
    iso, output = Path(iso), Path(output)
    supervisor.guard(8 * 1024**2)
    need(not output.exists(), "ISO validation output already exists")
    output.mkdir()
    prefix = output.name
    def run(label, argv, extra=1024**2):
        return supervisor.run(prefix + "-" + label, argv, extra=extra,
                              env_extra=env, timeout=60).read_text()
    digest = hashlib.sha256(iso.read_bytes()).hexdigest()
    catalog = run("catalog", ["xorriso", "-indev", str(iso), "-report_el_torito", "plain"])
    boot = parse_catalog(catalog)
    extent = parse_extent(run("extent", ["xorriso", "-indev", str(iso),
                          "-find", boot["image_path"], "-exec", "report_lba", "--"]), boot["image_path"])
    need(extent["lba"] == boot["lba2048"], "catalog/image LBA mismatch")
    need(extent["size"] == boot["sectors512"] * 512, "catalog/image sector count mismatch")
    offset = extent["lba"] * 2048
    need(offset + extent["size"] <= iso.stat().st_size, "EFI extent outside ISO")
    image = output / "efi.img"
    run("extract", ["xorriso", "-osirrox", "on", "-indev", str(iso),
                    "-extract", boot["image_path"], str(image)], extra=4 * 1024**2)
    data = image.read_bytes()
    need(len(data) == extent["size"], "extracted EFI size mismatch")
    with iso.open("rb") as f:
        f.seek(offset)
        need(f.read(len(data)) == data, "extracted EFI differs from ISO extent")
    fat = validate_fat(data)
    listing = run("boot-file", ["mdir", "-i", str(image), "::/efi/boot/bootx64.efi"])
    need("BOOTX64" in listing.upper() and "EFI" in listing.upper(), "EFI loader missing")
    loader = output / "BOOTX64.EFI"
    run("loader", ["mcopy", "-n", "-i", str(image), "::/efi/boot/bootx64.efi", str(loader)],
        extra=4 * 1024**2)
    validate_pe(loader.read_bytes())
    need(hashlib.sha256(iso.read_bytes()).hexdigest() == digest, "ISO changed during validation")
    result = dict(iso_sha256=digest, boot=boot, extent=extent, fat=fat,
                  efi_sha256=hashlib.sha256(data).hexdigest(),
                  loader_sha256=hashlib.sha256(loader.read_bytes()).hexdigest(),
                  loader_x86_64_efi_application=True)
    with (output / "result.json").open("x") as f:
        f.write(json.dumps(result, indent=2) + "\n")
    return result
