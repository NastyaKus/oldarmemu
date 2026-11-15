#!/usr/bin/env bash
# Create a microSD image (MBR + FAT32) and copy files onto it.
#   mksdcard.sh out.img SIZE_MB [files...]
set -euo pipefail
OUT="$1"; MB="$2"; shift 2
rm -f "$OUT"
truncate -s "${MB}M" "$OUT"
echo 'start=2048, type=c' | sfdisk -q "$OUT"
PART="$OUT.part"
truncate -s $(( (MB * 1024 * 1024) - 2048 * 512 )) "$PART"
mkfs.vfat -F 32 -n HD2SD "$PART" > /dev/null
for f in "$@"; do mcopy -i "$PART" "$f" ::/; done
dd if="$PART" of="$OUT" bs=512 seek=2048 conv=notrunc status=none
rm -f "$PART"
echo "created $OUT"
