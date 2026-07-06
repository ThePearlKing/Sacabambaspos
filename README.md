# SacabambaspOS

The most epic operating system in all of eternal existence.

Boots from a USB stick on modern hardware (UEFI: Mac, PC, laptop, virtual
machine) all the way into its **own kernel and interactive shell**. Runs
entirely **live / in memory** — it reads only the USB, writes to **no disk**,
and installs nothing. Pull the stick and your machine is untouched.

```
  USB power-on -> firmware runs \EFI\BOOT\BOOTX64.EFI          (stage 0, loader)
               -> splash: Sacabambaspis scaled-to-fit + centred,
                  "SacabambaspOS" title, version badge
               -> translucent boot console (bottom-left) replays the boot log
                  and follows the kernel handoff live
               -> loader places KERNEL.ELF + BIN.PAK, exits boot services, jumps
               -> kernel (stage 1): GDT/IDT, PIC+PIT, PS/2 + USB (xHCI)
                  keyboards, framebuffer console with color     (own code, no UEFI)
               -> kernel (stage 2): own page tables (W^X + NX), ring 3,
                  syscalls - real user programs run in /bin
               -> /SBOS/INIT.RC runs, then an interactive shell with live
                  syntax coloring - type help; try snake
```

## Keyboard on real hardware

Two independent stacks, both from scratch: PS/2 (laptop internals, boards
with legacy emulation) and a native **USB HID keyboard driver on xHCI** —
polled, no interrupts needed, hotplug supported, one external hub tier deep.
If a machine has either kind of keyboard port, typing should work.

## Secure Boot

The image is unsigned, so machines shipped with **Secure Boot enabled will
refuse to run it** ("Security Violation" or a silent fall-through to the next
boot entry). Disable Secure Boot in firmware setup first (usually
Security → Secure Boot → Disabled; Mac with T2: Startup Security Utility →
No Security). Booting the stick still writes nothing — re-enable Secure Boot
afterwards and the machine is exactly as before.

## What's built from scratch

| Piece | File | Notes |
|-------|------|-------|
| UEFI loader | `src/efi/boot.c` | own minimal EFI headers (`efi.h`), no gnu-efi; splash, boot console overlay, ELF kernel loader, ExitBootServices handoff |
| PE reloc stub | `src/efi/reloc.S` | makes the hand-built `.EFI` loadable by real firmware |
| Linker scripts | `src/efi/elf_*_efi.lds` | ELF→PE layout for x86-64 and ia32 |
| Kernel | `src/kernel/` | x86-64, static PIE: GDT/IDT/exceptions, PIC+PIT, PS/2 keyboard, framebuffer console (also mirrored to COM1) |
| USB keyboard | `src/kernel/usb.c` | native xHCI + HID boot protocol, fully polled, hotplug + hubs |
| Paging | `src/kernel/paging.c` | own 4-level page tables: identity map, kernel W^X, NX, user window |
| Ring 3 | `src/kernel/proc.c` + `entry.S` | syscall/sysret, ELF loader, user W^X, faults kill the process not the kernel |
| Userland | `src/user/` | freestanding C programs against `sbos.h` (`hello`, `primes`, `snake`, `crash`) |
| Shell | `src/kernel/shell.c` | live syntax coloring, history, line editing (arrows/Home/End/Del), builtins + `/bin` exec |
| Init script | `src/init/INIT.RC` | run by the kernel at boot, before the prompt |
| Program archive | `tools/mkpak.py` | packs user ELFs into `BIN.PAK`, loaded at boot (no filesystem driver needed yet) |
| Handoff contract | `src/shared/bootinfo.h` | loader → kernel: framebuffer, heap, RAM, ACPI RSDP, INIT.RC, BIN.PAK |
| Image → framebuffer | `tools/mkasset.py` | JPG → raw BGRA blob (`SBMP` format); no in-OS JPEG decoder |
| Console font | `tools/mkfont.py` | converts the host's PSF console font to an 8x16 C header at build time |
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

## Test in an emulator or VM (no hardware needed)

The bundled script uses QEMU (needs `qemu-system-x86` and `ovmf`):

```sh
./scripts/run-qemu.sh uefi gui     # opens a window; you should see the Sacabambaspis
./scripts/run-qemu.sh uefi         # headless -> build/shot-uefi.png
```

`build/sacabambaspos.img` is a plain raw disk image, so any VM with UEFI
firmware boots it too:

- **VirtualBox**: enable EFI (Settings → System → Enable EFI), convert with
  `VBoxManage convertfromraw build/sacabambaspos.img sacabambaspos.vdi`
- **VMware**: `firmware = "efi"` in the .vmx; attach the image (or convert
  with `qemu-img convert -O vmdk build/sacabambaspos.img sacabambaspos.vmdk`)
- **UTM / virt-manager / Hyper-V (Gen 2)**: attach the raw image as a disk on
  a UEFI/OVMF VM

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

Early days — under active development.

- [x] Stage 0: UEFI x86-64 boot + splash (Mac, PC, laptop, modern VM) — **verified in QEMU/OVMF**
- [x] Best-effort UEFI ia32 (`BOOTIA32.EFI`) for 32-bit EFI Macs (splash only)
- [x] Stage 1: kernel + interactive shell — boot console overlay, ELF loader,
      ExitBootServices, GDT/IDT, PIC/PIT, PS/2 keyboard, framebuffer console,
      INIT.RC, syntax-colored shell — **verified in QEMU/OVMF incl. keyboard input**
- [x] Native USB HID keyboard on xHCI (polled, hotplug, hubs) — PS/2-less boards type too
- [x] Own page tables + memory protection: W^X kernel, NX, guard pages
- [x] Stage 2: ring 3 userland — syscall/sysret, ELF loading, `/bin` programs
      (`hello`, `primes`, `snake`, `crash`), user faults can't touch the kernel —
      **verified in QEMU/OVMF (snake is playable)**
- [ ] Stage 3: real scheduler + multiple processes (the modern-kernel research
      list — capabilities, per-CPU, CoW storage — feeds this design)
- [ ] Filesystem driver (read the ESP directly instead of the BIN.PAK archive)
- [ ] Legacy BIOS path (16-bit MBR + VBE) for pre-UEFI PCs
- [ ] Configurable/customisable boot (background, image, options via a config file)

**Live-only for now — nothing is ever installed.** The installer comes later,
when the OS is complete and can be tried in-memory first.
