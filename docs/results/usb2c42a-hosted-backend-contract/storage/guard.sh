#!/bin/sh
# C4.2a storage admission (accepted plan): forecast 124,000,000 B, stop line 168,000,000 B, hard cap 200,000,000 B for this
# worktree (checkout + build + evidence, `du -sb`); Windows C: reserve 20 GiB (21,474,836,480 B) plus 1 GiB margin.
# Every step declares a projection taken from the unit-cost table. Before: refuse if tree + projection > stop line or C: is
# short. After: measure; if actual growth > 1.25 x projection, write final/STOP and refuse every later step until an
# investigation is recorded by a human (this script never deletes or moves anything).
# Usage: guard.sh <name> <projected-bytes> -- command...
set -u
name=$1; proj=$2; shift 3
root=/home/veasnawt/src/vos-usb2c42a
fin=$root/build/usb2c42a-1/final
log=$fin/storage-log.txt
STOPLINE=168000000; CAP=200000000; RESERVE=21474836480; MARGIN=1073741824
[ -e "$fin/STOP" ] && { echo "STOP file present: $(cat $fin/STOP)" | tee -a "$log"; exit 98; }
before=$(du -sb "$root" | cut -f1); free=$(df -B1 --output=avail /mnt/c | tail -1 | tr -d ' ')
echo "BEFORE $name: tree=$before projected=$proj c_free=$free" >> "$log"
if [ $((before + proj)) -gt $STOPLINE ] || [ $((before + proj)) -gt $CAP ] || [ $((free - proj)) -lt $((RESERVE + MARGIN)) ]; then
  echo "STOP: $name would cross a storage line (tree=$before proj=$proj c_free=$free)" | tee -a "$log" | tee "$fin/STOP"; exit 99
fi
"$@"; rc=$?
after=$(du -sb "$root" | cut -f1); growth=$((after - before)); limit=$((proj * 5 / 4))
echo "AFTER  $name: rc=$rc tree=$after growth=$growth projected=$proj limit(1.25x)=$limit" >> "$log"
if [ "$growth" -gt "$limit" ]; then
  echo "STOP: $name grew $growth B, over 1.25 x its projection ($proj); investigation required" | tee -a "$log" | tee "$fin/STOP"; exit 98
fi
if [ "$after" -gt $((STOPLINE * 85 / 100)) ]; then echo "NOTE: tree above 85% of the stop line" >> "$log"; fi
exit $rc
