# SacabambaspOS

The foundation of the most epic operating system in all of eternal existence.

Stage 0 is a **from-scratch bootloader** that boots on modern hardware (UEFI:
Mac, PC, laptop, virtual machine), detects the display, and paints the
**Sacabambaspis** full-screen. It runs entirely **live / in memory** — it reads
only the USB, writes to **no disk**, and installs nothing. Pull the stick and
your machine is untouched.

```
  USB power-on  ->  firmware runs  \EFI\BOOT\BOOTX64.EFI  ->  GOP framebuffer
                ->  load SACABASP.RAW  ->  scale-to-fit + centre  ->  idle
```

## What's built from scratch

| Piece | File | Notes |
|-------|------|-------|
| UEFI boot app | `src/efi/boot.c` | own minimal EFI headers (`efi.h`), no gnu-efi |
| PE reloc stub | `src/efi/reloc.S` | makes the hand-built `.EFI` loadable by real firmware |
| Linker scripts | `src/efi/elf_*_efi.lds` | ELF→PE layout for x86-64 and ia32 |
| Image → framebuffer | `tools/mkasset.py` | JPG → raw BGRA blob (`SBMP` format); no in-OS JPEG decoder |
| GPT disk builder | `tools/mkimage.py` | protective MBR + GPT + EFI System Partition, authored by hand |
| Build | `scripts/build.sh` | produces `build/sacabambaspos.img` |
| Emulator test | `scripts/run-qemu.sh` | boots the image in QEMU + OVMF, screenshots it |

## Build

Needs: `gcc`, `binutils` (`ld`/`objcopy`), `python3` + `Pillow`, `mtools`,
`dosfstools` (`mkfs.vfat`).

```sh
./scripts/build.sh                 # or: ./scripts/build.sh path/to/your.jpg
# -> build/sacabambaspos.img
```

## Test in an emulator (no hardware needed)

Needs `qemu-system-x86` and `ovmf`.

```sh
./scripts/run-qemu.sh uefi gui     # opens a window; you should see the Sacabambaspis
./scripts/run-qemu.sh uefi         # headless -> build/shot-uefi.png
```

## Burn to a USB stick

> **Read the drive name twice.** `dd` to the wrong device destroys it.
> This image is safe to *boot* (it never writes your PC), but *writing the
> image* to the wrong disk is what you must be careful about.

1. Plug in the USB stick.
2. Find its device node — it is the whole disk, e.g. `/dev/sdb`, **not** a
   partition like `/dev/sdb1`, and **not** your system disk:
   ```sh
   lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINT
   ```
   The USB stick usually shows `TRAN=usb` and matches its labelled size.
3. Unmount any of its partitions, then write:
   ```sh
   sudo dd if=build/sacabambaspos.img of=/dev/sdX bs=4M conv=fsync status=progress
   sync
   ```
   Replace `sdX` with your stick (e.g. `sdb`).
4. Move the stick to the target machine, open the firmware boot menu
   (commonly F12 / F9 / Esc; on a Mac hold **Option/⌥** at chime), pick the
   USB device. The Sacabambaspis appears.

## Scope / roadmap

- [x] UEFI x86-64 boot + image display (Mac, PC, laptop, modern VM) — **verified in QEMU/OVMF**
- [x] Best-effort UEFI ia32 (`BOOTIA32.EFI`) for 32-bit EFI Macs
- [ ] Legacy BIOS path (16-bit MBR + VBE) for pre-UEFI PCs
- [ ] Configurable/customisable boot (background, image, options via a config file)
- [ ] A kernel. An actual operating system. Eternal glory.

**Live-only for now — nothing is ever installed.** The installer comes later,
when the OS is complete and can be tried in-memory first.
