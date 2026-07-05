#!/usr/bin/env python3
"""
mkimage.py - assemble the bootable SacabambaspOS USB image.

Produces a GPT-partitioned disk image with an EFI System Partition (FAT).
The ESP already contains /EFI/BOOT/BOOTX64.EFI (+ BOOTIA32.EFI) and
/SACABASP.RAW. This script only lays down the protective MBR + GPT around a
pre-built ESP filesystem image.

GPT is used (not bare MBR) because Macs and modern UEFI firmware expect it.
An optional 440-byte BIOS boot blob can be embedded in the MBR for legacy
machines (hybrid boot); pass --mbr-boot to inject it.

Nothing here touches a real disk. It writes a single .img file you later `dd`
to the USB yourself.
"""
import sys, os, struct, zlib, argparse

SECTOR = 512
ESP_TYPE_GUID = b"\x28\x73\x2a\xc1\x1f\xf8\xd2\x11\xba\x4b\x00\xa0\xc9\x3e\xc9\x3b"  # C12A7328-... mixed-endian

def guid_random():
    # RFC 4122 v4: set version/variant bits so partition tools accept the GUID.
    # GPT stores GUIDs mixed-endian; version nibble lands in byte 7, variant in 8.
    b = bytearray(os.urandom(16))
    b[7] = (b[7] & 0x0F) | 0x40
    b[8] = (b[8] & 0x3F) | 0x80
    return bytes(b)

def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF

def build(esp_path, out_path, mbr_boot=None, align_mib=1):
    with open(esp_path, "rb") as f:
        esp = f.read()
    if len(esp) % SECTOR:
        esp += b"\x00" * (SECTOR - len(esp) % SECTOR)
    esp_sectors = len(esp) // SECTOR

    align = (align_mib * 1024 * 1024) // SECTOR   # e.g. 2048
    esp_start = align
    esp_end = esp_start + esp_sectors - 1          # inclusive

    # layout tail: backup GPT entries (32) + backup header (1)
    total_sectors = esp_end + 1 + 32 + 1
    last_lba = total_sectors - 1

    disk_guid = guid_random()
    part_guid = guid_random()

    # --- partition entry array (128 entries * 128 bytes) ---
    def part_entry(type_guid, uniq, first, last, attrs, name):
        nm = name.encode("utf-16-le")[:72].ljust(72, b"\x00")
        return struct.pack("<16s16sQQQ", type_guid, uniq, first, last, attrs) + nm
    entries = part_entry(ESP_TYPE_GUID, part_guid, esp_start, esp_end, 0, "SacabambaspOS ESP")
    entries += b"\x00" * 128 * 127   # remaining empty entries
    entries_crc = crc32(entries)

    def gpt_header(cur_lba, other_lba, entries_lba, first_usable, last_usable):
        hdr = bytearray(92)
        struct.pack_into("<8sIIIIQQQQ16sQIII", hdr, 0,
            b"EFI PART", 0x00010000, 92, 0, 0,
            cur_lba, other_lba, first_usable, last_usable,
            disk_guid, entries_lba, 128, 128, entries_crc)
        # header CRC computed over 92 bytes with crc field=0
        struct.pack_into("<I", hdr, 16, 0)
        c = crc32(bytes(hdr))
        struct.pack_into("<I", hdr, 16, c)
        return bytes(hdr)

    first_usable = 34
    last_usable = esp_end + 1 + 32 - 1  # last sector before backup header... but backup entries occupy last 33; usable ends before them
    last_usable = last_lba - 33

    primary_entries_lba = 2
    backup_entries_lba = last_lba - 32
    primary_hdr = gpt_header(1, last_lba, primary_entries_lba, first_usable, last_usable)
    backup_hdr  = gpt_header(last_lba, 1, backup_entries_lba, first_usable, last_usable)

    # --- protective MBR ---
    mbr = bytearray(SECTOR)
    if mbr_boot:
        with open(mbr_boot, "rb") as f:
            code = f.read()
        if len(code) > 440:
            raise SystemExit("MBR boot code >440 bytes")
        mbr[0:len(code)] = code
    # protective partition entry at 0x1BE, type 0xEE, spans whole disk.
    # With embedded BIOS boot code, mark it active: some BIOSes refuse to
    # boot an MBR with no active partition.
    prot_lba = 1
    prot_sz = min(total_sectors - 1, 0xFFFFFFFF)
    struct.pack_into("<B3sB3sII", mbr, 0x1BE,
        0x80 if mbr_boot else 0x00, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff",
        prot_lba, prot_sz)
    mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA

    # --- assemble ---
    img = bytearray(total_sectors * SECTOR)
    img[0:SECTOR] = mbr
    img[1*SECTOR:1*SECTOR+len(primary_hdr)] = primary_hdr
    img[primary_entries_lba*SECTOR:primary_entries_lba*SECTOR+len(entries)] = entries
    img[esp_start*SECTOR:esp_start*SECTOR+len(esp)] = esp
    img[backup_entries_lba*SECTOR:backup_entries_lba*SECTOR+len(entries)] = entries
    img[last_lba*SECTOR:last_lba*SECTOR+len(backup_hdr)] = backup_hdr

    with open(out_path, "wb") as f:
        f.write(img)
    print(f"[mkimage] {out_path}  {total_sectors} sectors  {len(img)} bytes "
          f"({len(img)//(1024*1024)} MiB)  ESP @LBA{esp_start}..{esp_end}")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("esp")
    ap.add_argument("out")
    ap.add_argument("--mbr-boot", default=None)
    a = ap.parse_args()
    build(a.esp, a.out, a.mbr_boot)
