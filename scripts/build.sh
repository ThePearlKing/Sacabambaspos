#!/usr/bin/env bash
# build.sh - build the SacabambaspOS bootable USB image from scratch.
#
# Output: build/sacabambaspos.img  ->  dd to a USB stick, boot any UEFI machine.
# Live/try-it only: it draws the Sacabambaspis and idles. Writes no disk.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
BUILD="$ROOT/build"
SRC="$ROOT/src"
mkdir -p "$BUILD"

SOURCE_IMG="${1:-$ROOT/sacabambaspis.jpg}"
ESP_MB=64

echo "==> [1/5] asset: $SOURCE_IMG -> SACABASP.RAW"
python3 tools/mkasset.py "$SOURCE_IMG" "$BUILD/SACABASP.RAW" --max-w 1920 --max-h 1080

CFLAGS="-ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar \
 -mno-red-zone -maccumulate-outgoing-args -fno-builtin -Wall -Wextra \
 -Wno-misleading-indentation -Wno-unused-parameter -fpic -Isrc/efi"
LDFLAGS="-nostdlib -znocombreloc -shared -Bsymbolic -T src/efi/elf_x86_64_efi.lds -e efi_main"

# The empty-.reloc PIC trick is only sound if the link produced no undefined
# symbols and no runtime relocations - verify, don't hope.
check_selfcontained() { # <.so>  -> fails loudly if the image needs a loader
  local so="$1"
  if [ -n "$(nm -D -u "$so" 2>/dev/null)" ]; then
    echo "ERROR: $so has undefined symbols:" >&2; nm -D -u "$so" >&2; return 1
  fi
  if readelf -rW "$so" 2>/dev/null | grep -qE 'R_(X86_64|386)_'; then
    echo "ERROR: $so has runtime relocations (breaks the empty-.reloc PE trick)" >&2
    return 1
  fi
}

echo "==> [2/5] UEFI x86-64: BOOTX64.EFI"
gcc $CFLAGS -c "$SRC/efi/boot.c" -o "$BUILD/boot.x64.o"
gcc $CFLAGS -c "$SRC/efi/reloc.S" -o "$BUILD/reloc.x64.o"
ld $LDFLAGS "$BUILD/boot.x64.o" "$BUILD/reloc.x64.o" -o "$BUILD/boot.x64.so"
check_selfcontained "$BUILD/boot.x64.so"
objcopy -j .text -j .data -j .reloc \
        --target efi-app-x86_64 "$BUILD/boot.x64.so" "$BUILD/BOOTX64.EFI"

echo "==> [3/5] UEFI ia32: BOOTIA32.EFI (best-effort, for 32-bit EFI Macs)"
rm -f "$BUILD/BOOTIA32.EFI"   # never ship a stale one from a previous build
# -malign-double: firmware lays out UINT64 struct fields 8-byte aligned on ia32
if gcc -m32 -malign-double $CFLAGS -c "$SRC/efi/boot.c" -o "$BUILD/boot.ia32.o" 2>/dev/null \
   && gcc -m32 -malign-double $CFLAGS -c "$SRC/efi/reloc.S" -o "$BUILD/reloc.ia32.o" 2>/dev/null; then
  ld -m elf_i386 -nostdlib -znocombreloc -shared -Bsymbolic \
     -T "$SRC/efi/elf_ia32_efi.lds" -e efi_main "$BUILD/boot.ia32.o" "$BUILD/reloc.ia32.o" -o "$BUILD/boot.ia32.so" 2>/dev/null \
  && check_selfcontained "$BUILD/boot.ia32.so" \
  && objcopy -j .text -j .data -j .reloc \
        --target efi-app-ia32 "$BUILD/boot.ia32.so" "$BUILD/BOOTIA32.EFI" 2>/dev/null \
    && echo "    ia32 ok" || { echo "    ia32 skipped (link)"; rm -f "$BUILD/BOOTIA32.EFI"; }
else
  echo "    ia32 skipped (no 32-bit libs)"
fi

echo "==> [4/5] ESP (FAT32, ${ESP_MB}MiB)"
ESP="$BUILD/esp.img"
rm -f "$ESP"
dd if=/dev/zero of="$ESP" bs=1M count=$ESP_MB status=none
# FAT32 labels cap at 11 chars, so the full name loses its "OS" here.
mkfs.vfat -F 32 -n SACABAMBASP -i 5ACABA05 "$ESP" >/dev/null   # fixed volume ID
mmd   -i "$ESP" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP" "$BUILD/BOOTX64.EFI"  ::/EFI/BOOT/BOOTX64.EFI
[ -f "$BUILD/BOOTIA32.EFI" ] && mcopy -i "$ESP" "$BUILD/BOOTIA32.EFI" ::/EFI/BOOT/BOOTIA32.EFI || true
mcopy -i "$ESP" "$BUILD/SACABASP.RAW" ::/SACABASP.RAW
mcopy -i "$ESP" "$BUILD/SACABASP.RAW" ::/EFI/BOOT/SACABASP.RAW

echo "==> [5/5] GPT disk image"
MBRBOOT=()
[ -f "$BUILD/mbr.bin" ] && MBRBOOT=(--mbr-boot "$BUILD/mbr.bin")
python3 tools/mkimage.py "$ESP" "$BUILD/sacabambaspos.img" "${MBRBOOT[@]}"

echo
echo "DONE -> build/sacabambaspos.img"
echo "Burn:  sudo dd if=build/sacabambaspos.img of=/dev/sdX bs=4M conv=fsync   (X = your USB, NOT a system disk)"
