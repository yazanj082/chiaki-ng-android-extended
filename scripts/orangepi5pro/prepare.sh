#!/bin/bash
# Unpacks Orange Pi's Android 12 box firmware for the Orange Pi 5 Pro into work/,
# for build-image.sh, and fetches the tools it needs (no root needed).
#
#   prepare.sh OrangePi5Pro_RK3588S_Android12-box_v1.0.1.tar.gz
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
W=$HERE/work
T=$W/tools/usr/bin
mkdir -p "$W/tools"

echo "== tools: Android image tools and U-Boot's mkimage from Arch Linux packages"
M=https://geo.mirror.pkgbuild.com/extra/os/x86_64
for p in android-tools uboot-tools; do
	f=$(curl -s "$M/" | grep -oE "\"$p-[0-9][^\"]*-x86_64\.pkg\.tar\.zst\"" | tr -d '"' | sort -V | tail -1)
	curl -sL "$M/$f" | tar --zstd -x -C "$W/tools" usr/bin
done

echo "== firmware"
tar xzf "$1" -C "$W" --wildcards '*.img'
img=$(ls "$W"/*.img | head -1)
python3 "$HERE/rkunpack.py" "$img" "$W/fw"
rm -f "$img"

echo "== logical partitions"
"$T/simg2img" "$W/fw/super.img" "$W/super.raw"
rm -rf "$W/parts"
mkdir -p "$W/parts"
"$T/lpunpack" "$W/super.raw" "$W/parts" >/dev/null
rm -f "$W/super.raw"

echo "== bootloader for SD cards"
python3 "$HERE/rkloader.py" "$W/fw/MiniLoaderAll.bin" "$W/idblock.bin"
echo "Ready, now run build-image.sh"
