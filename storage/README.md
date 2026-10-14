# Cartridge disk storage

This portable C layer stores standard 143,360-byte Apple II `.dsk` images in
LittleFS on a 4 MiB NOR device with 64 KiB erase sectors. The caller supplies
read, program, erase, and durable-sync callbacks. It contains no physical flash
commands and performs no hardware access by itself.

Call `a2_storage_init`, then `a2_storage_mount`. A mount failure is returned
without formatting. Only an explicit `a2_storage_format` erases/replaces storage.
Call `a2_storage_mkdir` for directories and `a2_storage_catalog` to enumerate
directories and valid disk images. `a2_disk_image_create` creates zeroed raw
sectors; it does not install DOS or create a bootable disk.

Zero-initialize image handles. Open an image, read/write bounded byte ranges,
then sync or close to commit writes. A Disk II track maps to offset `track*4096`
with length 4096. Keep filesystem/image objects at stable addresses and serialize
all calls. Unmount refuses while an image remains open.

The filesystem uses two static 256-byte caches and an 8-byte lookahead, plus a
256-byte cache in each image handle. Compile `a2_storage.c`, `vendor/littlefs/lfs.c`,
and `vendor/littlefs/lfs_util.c`; no heap is required (`LFS_NO_MALLOC` is tested).
Filesystem overhead and 64 KiB allocation blocks reduce usable image capacity
below the raw flash-size/image-size ratio.

Run `make -C storage test` from the repository root for the host NOR simulation with
AddressSanitizer/UndefinedBehaviorSanitizer. It enforces page alignment, 1-to-0
programming, and sector erases, and tests remount persistence, boundary writes,
one interrupted overwrite, and non-destructive failed mounts.

The wrapper and tests use MIT SPDX identifiers. Vendored LittleFS is unmodified
upstream v2.11.3, commit `6cb4e86540eca0d9ba62500a298385c9d863c8be`, under
BSD-3-Clause. Its original `LICENSE.md` and source notices are retained;
`vendor/littlefs/UPSTREAM.json` records exact URLs and SHA-256 hashes.
