#!/usr/bin/env bash
# Build the deterministic ext2 fixtures of the N3.3 tests (host side).
#
# usage: mkext2fixtures.sh OUTDIR FIXGEN
#
# For each image NAME it writes OUTDIR/NAME.img (made by mke2fs -d from a
# source tree OUTDIR/src-NAME) and OUTDIR/NAME.manifest, the independent
# description the kernel compares against. The manifest is built from the
# source tree with find, stat, readlink and sha256sum, and from dumpe2fs for
# the file-system geometry; nothing in it comes from parsing the image.
#
# Manifest lines (fields separated by one space, path last):
#   S <key> <value>                 file-system fact (block_size, blocks, inodes, inode_size,
#                                   blocks_per_group, inodes_per_group, first_block, groups)
#   F <size> <nlink> <mode> <sha256|-> <path>    regular file ("-": too large to hash whole)
#   R <offset> <length> <sha256> <path>          a hashed range of a regular file
#   D <children> <mode> <path>      directory; children excludes "." and ".."
#   L <length> <target> <path>      symbolic link
#   P <path>                        FIFO
#
# Images are reproducible: fixed UUID, hash seed, label and owner, fixed
# timestamps (E2FSPROGS_FAKE_TIME, E2FSCK_TIME, touch -d), sorted input. Every
# valid image must pass e2fsck -fn, and each is built twice, at least a second
# apart, and compared (so no wall-clock time can slip in unnoticed).
set -eu
OUT=$1 GEN=$2
# mke2fs and debugfs take their clock from E2FSPROGS_FAKE_TIME, e2fsck from E2FSCK_TIME (it stamps
# s_wtime and s_lastcheck); without both, e2fsck -D output would depend on the wall clock.
export E2FSPROGS_FAKE_TIME=1700000000 E2FSCK_TIME=1700000000 SOURCE_DATE_EPOCH=1700000000 LC_ALL=C TZ=UTC
STAMP=@1700000000

gen() { "$GEN" "$1" "$2" "$3"; }

common_tree() {
    local d=$1
    mkdir -p "$d"
    printf 'Hello from an ext2 file on VOS.\n' > "$d/hello.txt"
    ln "$d/hello.txt" "$d/hello-hardlink.txt"
    : > "$d/empty.txt"
    mkdir "$d/empty-dir"
    mkdir -p "$d/deep/a/b/c/d/e/f/g/h"
    printf 'deepest file\n' > "$d/deep/a/b/c/d/e/f/g/h/leaf.txt"
    mkdir -p "$d/dup/x" "$d/dup/y"
    printf 'name.txt in /\n' > "$d/name.txt"
    printf 'name.txt in /dup/x\n' > "$d/dup/x/name.txt"
    printf 'name.txt in /dup/y (a different file)\n' > "$d/dup/y/name.txt"
    gen "$d/l$(printf 'o%.0s' $(seq 1 252))ng" 777 long-name
    gen "$d/data.bin" 100000 data
    mkdir "$d/many"
    for i in $(seq -w 1 200); do gen "$d/many/file-$i" $((10#$i * 7 % 600 + 1)) "many-$i"; done
    ln -s hello.txt "$d/link-short"
    ln -s "deep/a/b/c/d/e/f/g/h/../../../../../../../../../$(printf 'x%.0s' $(seq 1 40))/target" "$d/link-long"
    ln -s deep/a "$d/link-dir"
    mkfifo "$d/pipe"
    chmod 0755 "$d/data.bin"
}

boundary_files() {      # boundary sizes for block size $2, and a sparse file reaching the triple-indirect map
    local d=$1/sizes bs=$2 p=$(( $2 / 4 ))
    mkdir -p "$d"
    for n in 1 $((bs - 1)) $bs $((bs + 1)) $((12 * bs)) $((12 * bs + 1)) $(((12 + p) * bs)) $(((12 + p) * bs + 1)); do
        gen "$d/size-$n" $n "size-$bs-$n"
    done
    local t=$(( (12 + p + p * p) * bs ))          # first byte mapped by the triple-indirect block
    truncate -s $((t + 2 * bs)) "$1/sparse"
    gen "$1/.tail" $bs "sparse-tail-$bs"
    dd if="$1/.tail" of="$1/sparse" bs=$bs seek=$(( t / bs + 1 )) conv=notrunc status=none
    rm "$1/.tail"
}

manifest() {            # manifest SRC IMG OUT
    local src=$1 img=$2 out=$3
    {
        local bs blocks
        bs=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/^Block size:/ {gsub(/ /, "", $2); print $2}')
        blocks=$(dumpe2fs -h "$img" 2>/dev/null | awk -F: '/^Block count:/ {gsub(/ /, "", $2); print $2}')
        dumpe2fs -h "$img" 2>/dev/null | awk -F: '
            /^Block size:/       { gsub(/ /, "", $2); print "S block_size " $2 }
            /^Block count:/      { gsub(/ /, "", $2); print "S blocks " $2 }
            /^Inode count:/      { gsub(/ /, "", $2); print "S inodes " $2 }
            /^Inode size:/       { gsub(/ /, "", $2); print "S inode_size " $2 }
            /^Blocks per group:/ { gsub(/ /, "", $2); print "S blocks_per_group " $2 }
            /^Inodes per group:/ { gsub(/ /, "", $2); print "S inodes_per_group " $2 }
            /^First block:/      { gsub(/ /, "", $2); print "S first_block " $2 }'
        echo "S groups $(dumpe2fs "$img" 2>/dev/null | grep -c '^Group [0-9]')"
        echo "D $(( $(ls -A "$src" | wc -l) + 1 )) 755 /"
        echo "D 0 700 /lost+found"
        (cd "$src" && find . -mindepth 1 | sort) | while read -r rel; do
            local p=${rel#.} f="$src/${rel#./}"
            case $(stat -c %F "$f") in
            directory)
                echo "D $(ls -A "$f" | wc -l) $(stat -c %a "$f") $p";;
            "symbolic link")
                local t; t=$(readlink "$f"); echo "L ${#t} $t $p";;
            fifo)
                echo "P $p";;
            *)
                local size; size=$(stat -c %s "$f")
                if [ "$size" -gt $((16 * 1024 * 1024)) ]; then
                    echo "F $size $(stat -c %h "$f") $(stat -c %a "$f") - $p"
                    echo "R 0 $bs $(head -c "$bs" "$f" | sha256sum | cut -d' ' -f1) $p"
                    local mid=$(( size / 2 / bs * bs ))
                    echo "R $mid $bs $(tail -c +$((mid + 1)) "$f" | head -c "$bs" | sha256sum | cut -d' ' -f1) $p"
                    local last=$(( size - 2 * bs ))
                    echo "R $last $((2 * bs)) $(tail -c $((2 * bs)) "$f" | sha256sum | cut -d' ' -f1) $p"
                else
                    echo "F $size $(stat -c %h "$f") $(stat -c %a "$f") $(sha256sum < "$f" | cut -d' ' -f1) $p"
                fi;;
            esac
        done
    } > "$out"
}

build() {               # build NAME SRC SIZE MKE2FS_ARGS...   (SIZE with a k or M suffix)
    local name=$1 src=$2 size=$3; shift 3
    local img=$OUT/$name.img
    [ -n "$src" ] && find "$src" -exec touch -h -d $STAMP {} +
    for i in 1 2; do
        [ $i = 2 ] && sleep 1.1
        rm -f "$img.$i"
        mke2fs -q -F -t ext2 -m 0 -E root_owner=0:0,hash_seed=564f5333-0000-4000-8000-0000000000ff \
            -L "vos-$name" "$@" ${src:+-d "$src"} "$img.$i" "$size"
    done
    cmp -s "$img.1" "$img.2" || { echo "mkext2fixtures: $name is not reproducible" >&2; exit 1; }
    e2fsck -fn "$img.1" >/dev/null || { echo "mkext2fixtures: $name fails e2fsck" >&2; exit 1; }
    mv -f "$img.1" "$img"
    rm -f "$img.2"
    chmod a-w "$img"
    if [ -n "$src" ]; then
        manifest "$src" "$img" "$OUT/$name.manifest"
    else
        manifest "$OUT/src-empty-none" "$img" "$OUT/$name.manifest"
    fi
}

rm -rf "$OUT"
mkdir -p "$OUT"

common_tree "$OUT/src-1k"; boundary_files "$OUT/src-1k" 1024
build ext2-1k "$OUT/src-1k" 8M -b 1024 -g 1024 -I 256 -N 512 -U 564f5333-0000-4000-8000-000000000001

# The same file system after e2fsck -D: directories rebuilt as hashed (htree) indexes, which a
# linear reader must still read exactly (dx_root hides behind "..", interior nodes are empty records).
for i in 1 2; do
    [ $i = 2 ] && sleep 1.1
    cp "$OUT/ext2-1k.img" "$OUT/ext2-htree.img.$i"; chmod u+w "$OUT/ext2-htree.img.$i"
    rc=0; e2fsck -fyD "$OUT/ext2-htree.img.$i" >/dev/null 2>&1 || rc=$?
    [ $rc -le 1 ] || { echo "mkext2fixtures: e2fsck -D failed ($rc)" >&2; exit 1; }
done
cmp -s "$OUT/ext2-htree.img.1" "$OUT/ext2-htree.img.2" || { echo "mkext2fixtures: ext2-htree is not reproducible" >&2; exit 1; }
e2fsck -fn "$OUT/ext2-htree.img.1" >/dev/null || { echo "mkext2fixtures: ext2-htree fails e2fsck" >&2; exit 1; }
mv -f "$OUT/ext2-htree.img.1" "$OUT/ext2-htree.img"; rm -f "$OUT/ext2-htree.img.2"; chmod a-w "$OUT/ext2-htree.img"
manifest "$OUT/src-1k" "$OUT/ext2-htree.img" "$OUT/ext2-htree.manifest"

common_tree "$OUT/src-2k"; boundary_files "$OUT/src-2k" 2048
build ext2-2k "$OUT/src-2k" 4M -b 2048 -g 1024 -I 128 -N 512 -U 564f5333-0000-4000-8000-000000000002

common_tree "$OUT/src-4k"; boundary_files "$OUT/src-4k" 4096
build ext2-4k "$OUT/src-4k" 16M -b 4096 -g 1024 -I 256 -N 512 -U 564f5333-0000-4000-8000-000000000004

mkdir -p "$OUT/src-empty-none"
build ext2-empty "" 512k -b 1024 -I 256 -N 32 -U 564f5333-0000-4000-8000-000000000005

mkdir -p "$OUT/src-nofeat/sub/dir"
printf 'no features\n' > "$OUT/src-nofeat/readme.txt"
"$GEN" "$OUT/src-nofeat/sub/dir/data" 30000 nofeat
ln -s ../readme.txt "$OUT/src-nofeat/sub/up"
build ext2-nofeat "$OUT/src-nofeat" 2M -b 1024 -g 1024 -I 128 -N 64 -O none -U 564f5333-0000-4000-8000-000000000006

(cd "$OUT" && sha256sum *.img > SHA256SUMS)
ls -l "$OUT"/*.img "$OUT"/*.manifest
