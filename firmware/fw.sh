#!/bin/zsh
# Builds the Gochi firmware and puts it in docs/install/fw/ next to the install page. "Install all 8" on that page erases the whole
# box and installs this firmware (esptool-js), then sends the clock, rotation and all 8 packs.
# Version = first 12 hex chars of sha256(trumpgochi.ino): any firmware change gets a new version, no manual bump.
# Run on the Mac from this folder after changing trumpgochi.ino:  ./fw.sh
set -e
cd "${0:A:h}"
SRC=trumpgochi
OUT=../docs/install/fw
B=/tmp/trumpgochi_build
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,USBMode=hwcdc"
VER=$(shasum -a 256 $SRC/trumpgochi.ino | cut -c1-12)
TMPDIR=/tmp/ arduino-cli compile --fqbn "$FQBN" --build-path $B $SRC
mkdir -p $OUT
rm -f $OUT/*.bin(N)
cp $B/trumpgochi.ino.bootloader.bin $OUT/bootloader.bin
cp $B/trumpgochi.ino.partitions.bin $OUT/partitions.bin
cp $B/boot_app0.bin $OUT/boot_app0.bin
cp $B/trumpgochi.ino.bin $OUT/app.bin
# esptool-js 0.7.0 (Apache-2.0), the same copy the booth setup page uses
ESPT="../QR Photo Booth/web/public/fw"
[[ -f $OUT/esptool-0.7.0.js ]] || cp "$ESPT/esptool-0.7.0.js" "$ESPT/esptool-LICENSE.txt" $OUT/
cat > $OUT/manifest.json <<J
{"version":"$VER","parts":[{"addr":0,"file":"bootloader.bin"},{"addr":32768,"file":"partitions.bin"},{"addr":57344,"file":"boot_app0.bin"},{"addr":65536,"file":"app.bin"}]}
J
grep -q '^0x0 .*bootloader' $B/flash_args && grep -q '^0x8000 .*partitions' $B/flash_args && grep -q '^0xe000 boot_app0' $B/flash_args && grep -q '^0x10000 ' $B/flash_args \
  || { echo "flash_args offsets changed — update manifest in fw.sh"; cat $B/flash_args; exit 1; }
echo "firmware $VER → $OUT"
