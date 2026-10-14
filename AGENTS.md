# Working on this repository

This is a standalone Chromatic project. Start with [README.md](README.md).
Builds need external ESP-IDF 5.3 and FPGA tools, not another application checkout.

## Layout

- `mcu/main/`: emulator, display/input, cartridge storage, networking and SSH.
- `fpga/rtl/` and `fpga/board/`: original controller and tested board constraints.
- `storage/`: flash/filesystem, sector journals and DOS catalog parsing.
- `basic/`: original BASIC programs. `tools/`: build/configuration/check helpers.
- `docs/`: build, installation/recovery, usage, verification and sharing guides.

## Checks

Run `python3 tools/check.py` from the root for public host tests and available
FPGA simulations. `--help` lists strict checks and optional SSH/private-ROM tests.
No default check programs hardware or needs Apple ROMs. GitHub CI runs the strict
public suite, including localhost OpenSSH tests.

Use focused checks during edits; run the public suite before a release. Keep
changes small and preserve the documented storage, UART and HTTP contracts.
Do not reformat or replace vendored code without a specific reason; retain its
notices and provenance.

## Private inputs and hardware

Use `python3 tools/configure.py` for private settings. ROMs, disk images, raw
backups and provisioned firmware stay in ignored build/private directories.
Run `python3 tools/check_public.py` before publishing a Git checkout.

Read [installation and recovery](docs/install.md) before hardware work. Select
the intended programmer explicitly and retain its factory backup. A build or
simulation is not authorization to flash, format a cartridge or reset a device.
Document physical observations separately from host tests; keep unverified
hardware support out of feature claims. USB/Bluetooth keyboards and seamless
cartridge hot swapping are not implemented.
