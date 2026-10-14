#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build original Verilog for the Chromatic with the open Gowin toolchain.

This only creates files; it never opens or programs a USB device.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PART = "GW5A-EV25UG256CC1/I0"
FAMILY = "GW5A-25A"


def parameter_value(value):
    name, separator, number = value.partition("=")
    if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$]*", name):
        raise argparse.ArgumentTypeError("parameter must be NAME=INTEGER with a Verilog identifier")
    if not re.fullmatch(r"[+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+)", number):
        raise argparse.ArgumentTypeError("parameter value must be a decimal or hexadecimal integer")
    radix = 16 if number.lstrip("+-").lower().startswith("0x") else 10
    return name, int(number, radix)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run(command, log, directory, environment, timeout):
    print("Running " + Path(command[0]).name, flush=True)
    with log.open("w") as output:
        process = subprocess.Popen(
            command, cwd=directory, env=environment, stdout=output,
            stderr=subprocess.STDOUT, start_new_session=True,
        )
        try:
            code = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise
    if code:
        print(log.read_text(errors="replace")[-8000:], file=sys.stderr)
        raise SystemExit(f"Build stopped with exit {code}; see {log}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path, nargs="*", help="original Verilog inputs")
    parser.add_argument("--top", default="toolchain_smoke")
    parser.add_argument("--cst", type=Path)
    parser.add_argument("--out", type=Path, default=ROOT / "build/tools/smoke")
    parser.add_argument("--suite", type=Path, default=ROOT / "build/tools/oss-cad-suite")
    parser.add_argument("--frequency", type=float, default=33.554432, help="MHz")
    parser.add_argument("--timeout", type=int, default=600, help="seconds per tool")
    parser.add_argument("--param", action="append", type=parameter_value, default=[],
                        metavar="NAME=INTEGER", help="override a top parameter (repeatable)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.frequency) or args.frequency <= 0:
        parser.error("--frequency must be a positive finite number")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_$]*", args.top):
        parser.error("--top must be a Verilog identifier")
    parameters = dict(args.param)
    if len(parameters) != len(args.param):
        parser.error("each --param name must be unique")
    sources = [p.resolve(strict=True) for p in args.sources] or [ROOT / "tools/smoke.v"]
    cst = (args.cst or ROOT / "tools/smoke.cst").resolve(strict=True)
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    suite = args.suite.resolve(strict=True)
    environment = os.environ.copy()
    environment["PATH"] = str(suite / "bin") + os.pathsep + environment.get("PATH", "")
    for name, leaf in [("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache")]:
        path = ROOT / "build/tools/runtime" / leaf
        path.mkdir(parents=True, exist_ok=True)
        environment[name] = str(path)
    # The suite's nextpnr wrapper overwrites XDG paths with hard-coded HOME
    # locations. Invoke its native executable with the same runtime resources.
    environment["PYTHONHOME"] = str(suite)
    environment["PYTHONNOUSERSITE"] = "1"
    environment["QT_PLUGIN_PATH"] = str(suite / "lib/qt6/plugins")
    environment["QT_LOGGING_RULES"] = "*=false"
    environment["XDG_DATA_DIRS"] = str(suite / "share")
    environment["XDG_CONFIG_DIRS"] = str(suite)
    synthesis = directory / "synth.json"
    routed = directory / "routed.json"
    bitstream = directory / "design.fs"
    manifest = directory / "build.json"
    for output in [synthesis, routed, bitstream, manifest]:
        output.unlink(missing_ok=True)
    script = directory / "synth.ys"
    script.write_text(
        "read_verilog " + " ".join(json.dumps(str(p)) for p in sources) + "\n"
        + "".join(f"chparam -set {name} {value} {args.top}\n" for name, value in parameters.items())
        + f"synth_gowin -top {args.top} -family gw5a -strict-gw5a-dffs -json {json.dumps(str(synthesis))}\n"
    )
    commands = [
        [str(suite / "bin/yosys"), "-T", "-s", str(script)],
        [str(suite / "libexec/nextpnr-himbaechel"), "--device", PART,
         "--vopt", f"family={FAMILY}", "--vopt", f"cst={cst}",
         "--json", str(synthesis), "--write", str(routed), "--freq", str(args.frequency)],
        [str(suite / "bin/gowin_pack"), "-d", FAMILY, "-o", str(bitstream), str(routed)],
    ]
    for command, name in zip(commands, ["synthesis", "place-route", "pack"]):
        run(command, directory / f"{name}.log", directory, environment, args.timeout)
    if not bitstream.is_file() or bitstream.stat().st_size == 0:
        raise SystemExit("Packer produced no bitstream")
    manifest.write_text(json.dumps({
        "part": PART, "family": FAMILY, "top": args.top, "frequency_mhz": args.frequency,
        "parameters": parameters,
        "inputs": {str(p): digest(p) for p in [*sources, cst]},
        "commands": commands, "bitstream": str(bitstream),
        "bitstream_sha256": digest(bitstream), "bitstream_bytes": bitstream.stat().st_size,
        "hardware_programmed": False,
    }, indent=2) + "\n")
    print(f"Built {bitstream} ({bitstream.stat().st_size} bytes)", flush=True)


if __name__ == "__main__":
    main()
