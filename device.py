#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Guard an explicitly selected Chromatic programmer before FPGA operations.

Flash detection, backup, and installation interrupt the running SRAM image and
reload installed firmware. Restore reloads whichever firmware is installed; it
does not restore a backup. Run status, then copy its serial into --serial.
Only one GWU2X programmer may be connected, because factory serials can repeat.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def scan_programmers(program):
    """Read the loader's descriptor-only USB inventory; never touch JTAG."""
    output = subprocess.check_output([program, "--scan-usb"], text=True,
                                     stderr=subprocess.STDOUT, timeout=20)
    header = None
    devices = []
    columns = ("Bus", "device", "vid:pid", "probe_type", "manufacturer", "serial", "product")
    for line in ANSI.sub("", output).splitlines():
        if re.fullmatch(r"\s*Bus\s+device\s+vid:pid\s+probe_type\s+manufacturer\s+serial\s+product\s*", line):
            header = [line.index(column) for column in columns]
            continue
        match = re.match(r"^\s*(\d+)\s+(\d+)\s+(?:0x)?([\da-fA-F]{4}):(?:0x)?([\da-fA-F]{4})\s+", line)
        if not match:
            continue
        vendor, product = int(match[3], 16), int(match[4], 16)
        if (vendor, product) != (0x33AA, 0x0120):
            continue
        if header is None:
            raise RuntimeError("Unrecognized USB inventory format; cannot verify programmer identity")
        fields = [line[start:end].strip() for start, end in zip(header, header[1:] + [len(line)])]
        if fields[3].lower() != "gwu2x" or not fields[5]:
            raise RuntimeError("Incomplete GWU2X identity in USB inventory")
        devices.append({"programmer_serial": fields[5], "usb_bus": int(match[1]),
                        "usb_address": int(match[2]), "product": fields[6]})
    if header is None:
        raise RuntimeError("USB inventory header missing; check loader USB-scan support and permissions")
    return devices


def selected_device(devices, serial):
    # Do not rely on a generic factory serial to distinguish multiple consoles.
    if len(devices) != 1:
        raise RuntimeError("Connect exactly one GWU2X programmer before this operation")
    if devices[0]["programmer_serial"] != serial:
        raise RuntimeError("Programmer serial does not match --serial; run status again")
    return devices[0]


def loader_path(explicit):
    if explicit:
        program = shutil.which(str(explicit))
    else:
        program = shutil.which("openFPGALoader")
        if not program:
            bundled = ROOT / "build/tools/oss-cad-suite/bin/openFPGALoader"
            program = str(bundled) if bundled.is_file() else None
    if not program:
        raise RuntimeError("openFPGALoader is unavailable; install it or pass --loader PATH")
    return program


def positive_size(value):
    size = int(value)
    if size <= 0:
        raise argparse.ArgumentTypeError("size must be greater than zero")
    return size


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="exact programmer serial reported by status; required for operations")
    parser.add_argument("--loader", type=Path, help="openFPGALoader executable (default: PATH, then local OSS CAD Suite)")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("restore", help="reload the currently installed FPGA firmware")
    commands.add_parser("detect-flash", help="identify configuration flash; interrupts SRAM")
    for name, help_text in (
            ("load", "load an image into volatile SRAM"),
            ("install", "persistently install an FPGA image; interrupts SRAM")):
        image_command = commands.add_parser(name, help=help_text)
        image_command.add_argument("image", type=Path)
        image_command.add_argument("--sha256", required=True)
    backup = commands.add_parser("backup", help="dump configuration flash; interrupts SRAM")
    backup.add_argument("output", type=Path)
    backup.add_argument("--size", required=True, type=positive_size, metavar="BYTES")
    args = parser.parse_args(argv)
    if args.command != "status" and not args.serial:
        parser.error("--serial SERIAL is required; run status and confirm the connected Chromatic first")
    try:
        if args.command in ("load", "install"):
            if not re.fullmatch(r"[0-9a-fA-F]{64}", args.sha256):
                raise RuntimeError("--sha256 must contain exactly 64 hexadecimal digits")
            actual = hashlib.sha256(args.image.read_bytes()).hexdigest()
            if actual != args.sha256.lower():
                raise RuntimeError("Image SHA-256 mismatch")
            suffix = args.image.suffix.lower()
            if suffix not in ((".fs",) if args.command == "load" else (".fs", ".bin")):
                raise RuntimeError("SRAM load requires .fs; installation accepts .fs or a raw .bin backup")
        if args.command == "backup":
            output = args.output.absolute()
            if output.exists() or output.is_symlink():
                raise RuntimeError(f"Backup output already exists: {output}")
        program = loader_path(args.loader)
        devices = scan_programmers(program)
        if args.command == "status":
            print(json.dumps({"programmers": devices}, indent=2))
            return
        selected_device(devices, args.serial)
        command = [program, "--cable", "gwu2x"]
        timeout = 120
        if args.command == "restore":
            command += ["--reset"]
        elif args.command == "detect-flash":
            command += ["--detect", "--write-flash"]
            timeout = 300
        elif args.command == "backup":
            # Reserve without clobbering an existing file, including a symlink.
            with output.open("xb"):
                pass
            command += ["--dump-flash", "--offset", "0", "--file-size",
                        str(args.size), str(output)]
            timeout = 300
        elif args.command == "install":
            command += ["--write-flash", "--offset", "0", "--verify", "--reset",
                        "--file-type", suffix[1:], str(args.image.resolve())]
            if suffix == ".bin":
                # A raw factory backup covers the external configuration flash.
                command.insert(3, "--external-flash")
            timeout = 300
        elif args.command == "load":
            command += ["--write-sram", "--file-type", "fs", str(args.image.resolve())]
        subprocess.run(command, check=True, timeout=timeout)
        if args.command == "backup":
            actual_size = output.stat().st_size
            if actual_size != args.size:
                raise RuntimeError(f"Incomplete backup: expected {args.size} bytes, "
                                   f"received {actual_size}; retained at {output}")
            print(f"Backup saved to {output} ({actual_size} bytes)")
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Chromatic operation failed: {error}\n")


if __name__ == "__main__":
    main()
