#!/usr/bin/env bash
# U0 (experimental, not part of the build): UEFI-only GRUB ISOs of the existing,
# unchanged x86_64 kernels, for OVMF feasibility tests. See docs/plans/uefi.md.
#
# Needs the existing BIOS build trees (make iso; make VOS_TEST=1 iso), GRUB's
# x86_64-efi platform files (grub-efi-amd64-bin) and mtools on PATH.
#   GRUB_EFI_DIR  x86_64-efi directory (default /usr/lib/grub/x86_64-efi)
#   OUT           output directory (default build/x86_64-uefi-u0)
# Output: $OUT/vos-uefi-prod.iso, $OUT/vos-uefi-test.iso, and "-nomod" variants
# without the module line (diagnostic only: see U0-B1). Nothing is written
# anywhere else; no disk, partition or firmware variable is touched.
set -eu
cd "$(dirname "$0")/.."
EFI=${GRUB_EFI_DIR:-/usr/lib/grub/x86_64-efi}
OUT=${OUT:-build/x86_64-uefi-u0}
[ -f "$EFI/kernel.img" ] && [ -f "$EFI/multiboot2.mod" ] || { echo "no GRUB x86_64-efi files in $EFI" >&2; exit 1; }
command -v mformat >/dev/null || { echo "mtools (mformat) not on PATH" >&2; exit 1; }
mkdir -p "$OUT"
for img in prod test; do
    src=build/x86_64/isodir
    [ $img = test ] && src=build/x86_64-test/isodir
    [ -f $src/boot/vos.bin ] || { echo "missing $src (run the BIOS iso target first)" >&2; exit 1; }
    for variant in "" -nomod; do
        dst=$OUT/isodir-$img$variant
        rm -rf "$dst"
        cp -r $src "$dst"
        {
            echo 'serial --unit=0 --speed=38400'
            echo 'terminal_output console serial'
            echo 'terminal_input console serial'
            echo 'echo "grub: platform ${grub_platform} cpu ${grub_cpu}"'
            echo 'lsefisystab'
            echo 'lsmmap'
            sed 's|^    boot$|    echo "grub: kernel and module loaded, booting"\n    boot|' $src/boot/grub/grub.cfg
        } > "$dst/boot/grub/grub.cfg"
        [ -z "$variant" ] || sed -i '/module2/d' "$dst/boot/grub/grub.cfg"
        grub-mkrescue -d "$EFI" -o "$OUT/vos-uefi-$img$variant.iso" "$dst" 2>&1 | tail -1
    done
done
sha256sum $OUT/isodir-*/boot/vos.bin build/x86_64/vos.bin build/x86_64-test/vos.bin
