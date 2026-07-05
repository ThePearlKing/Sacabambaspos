#!/usr/bin/env bash
# run-qemu.sh - boot build/sacabambaspos.img in QEMU and grab a screenshot.
#   ./scripts/run-qemu.sh uefi   -> UEFI firmware (OVMF)  [default]
#   ./scripts/run-qemu.sh bios   -> legacy BIOS (SeaBIOS)
#   ./scripts/run-qemu.sh <mode> gui  -> interactive window instead of headless
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:-uefi}"
GUI="${2:-headless}"
IMG="build/sacabambaspos.img"
SHOT="build/shot-$MODE.ppm"
[ -f "$IMG" ] || { echo "no $IMG - run scripts/build.sh first"; exit 1; }

find_ovmf() {
  for p in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
           /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd \
           /usr/share/edk2/x64/OVMF_CODE.fd; do
    [ -f "$p" ] && { echo "$p"; return; }
  done
}

QEMU=qemu-system-x86_64
COMMON=(-m 512 -drive file="$IMG",format=raw,if=none,id=usb0
        -device qemu-xhci -device usb-storage,drive=usb0
        -vga std -no-reboot)

ARGS=("${COMMON[@]}")
if [ "$MODE" = uefi ]; then
  OVMF="$(find_ovmf || true)"
  [ -n "$OVMF" ] || { echo "OVMF firmware not found (install 'ovmf')"; exit 1; }
  # writable varstore copy
  VARS=/usr/share/OVMF/OVMF_VARS_4M.fd; [ -f "$VARS" ] || VARS=/usr/share/OVMF/OVMF_VARS.fd
  cp -f "$VARS" build/OVMF_VARS.fd 2>/dev/null || :
  ARGS+=(-drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF")
  [ -f build/OVMF_VARS.fd ] && ARGS+=(-drive if=pflash,format=raw,unit=1,file=build/OVMF_VARS.fd)
  echo "firmware: OVMF ($OVMF)"
else
  echo "firmware: SeaBIOS (legacy)"
fi

if [ "$GUI" = gui ]; then
  exec "$QEMU" "${ARGS[@]}"
fi

# headless: emulate VGA, drive monitor over stdio, screendump after boot settles
echo "booting headless; screenshot -> $SHOT"
( sleep 12; echo "screendump $SHOT"; sleep 2; echo quit ) \
  | "$QEMU" "${ARGS[@]}" -display none -monitor stdio -serial null >/dev/null 2>&1 || true

if [ -f "$SHOT" ]; then
  python3 - "$SHOT" "build/shot-$MODE.png" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])
print("saved", sys.argv[2])
PY
else
  echo "no screenshot produced"; exit 1
fi
