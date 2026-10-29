#!/bin/sh
# Storage admission for one C3b.2 step: build tree below the 566,231,040 B stop
# line with the step's projection, and Windows C: above the 20 GiB reserve.
# Usage: guard.sh <name> <projected-bytes> -- command...
set -u
name=$1; proj=$2; shift 3
root=/home/veasnawt/src/vos-usb2c3b2/build/usb2c3b2-1
used=$(du -sb "$root" | cut -f1); free=$(df -B1 --output=avail /mnt/c | tail -1 | tr -d ' ')
echo "storage before $name: tree=$used c_free=$free projected=$proj" >> "$root/final/storage-log.txt"
if [ $((used + proj)) -gt 566231040 ] || [ $((free - proj)) -lt 21474836480 ]; then
  echo "STOP: $name would cross a storage line (tree=$used proj=$proj c_free=$free)" | tee -a "$root/final/storage-log.txt"; exit 99
fi
"$@"; rc=$?
echo "$name exit $rc tree_after=$(du -sb "$root" | cut -f1)" >> "$root/final/storage-log.txt"
exit $rc
