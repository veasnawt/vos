#!/bin/sh
# C4.1 storage admission. Stop line 224 MiB (234,881,024 B) and cap 256 MiB (268,435,456 B) (raised from 160/200 MiB by owner decision 2026-10-30) for this worktree
# (checkout + build + evidence, `du -sb`), set by the C4.1 forecast; C: reserve 20 GiB unchanged. These limits are
# new for C4.1: the C3b.2 600 MiB cap is not reused. Crossing the stop line means STOP and ask.
# Usage: guard.sh <name> <projected-bytes> -- command...
set -u
name=$1; proj=$2; shift 3
root=/home/veasnawt/src/vos-usb2c41
log=$root/build/usb2c41-1/final/storage-log.txt
used=$(du -sb "$root" | cut -f1); free=$(df -B1 --output=avail /mnt/c | tail -1 | tr -d ' ')
echo "storage before $name: tree=$used c_free=$free projected=$proj" >> "$log"
if [ $((used + proj)) -gt 234881024 ] || [ $((free - proj)) -lt 21474836480 ]; then
  echo "STOP: $name would cross a storage line (tree=$used proj=$proj c_free=$free)" | tee -a "$log"; exit 99
fi
"$@"; rc=$?
echo "$name exit $rc tree_after=$(du -sb "$root" | cut -f1)" >> "$log"
exit $rc
