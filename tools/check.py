#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check a source-only checkout without hardware, private settings or Apple ROMs.

Optional: --mbedtls /path/to/mbedtls-3.6 enables the localhost OpenSSH suite.
Optional: --rom FILE --disk-rom FILE --disk-image FILE enables private-ROM tests.
All generated files stay under ignored build directories; disk inputs are copied.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MCU = ROOT / 'mcu'
BUILD = ROOT / 'build/check'


def run(command, *, cwd=ROOT, timeout=60, env=None):
    subprocess.run([str(item) for item in command], cwd=cwd, check=True,
                   timeout=timeout, env=env)


def sanitizer_flags(compiler, required, disabled):
    if disabled:
        return []
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    source, binary = BUILD / 'sanitizer_probe.c', BUILD / 'sanitizer_probe'
    source.write_text('int main(void) { return 0; }\n')
    with (BUILD / 'sanitizer_probe.log').open('w') as log:
        try:
            subprocess.run([*compiler, *flags, str(source), '-o', str(binary)],
                           check=True, stdout=log, stderr=log, timeout=30)
            subprocess.run([str(binary)], check=True, stdout=log, stderr=log, timeout=10)
        except (OSError, subprocess.SubprocessError):
            if required:
                raise SystemExit('Address/undefined sanitizers unavailable; see build/check/sanitizer_probe.log')
            print('SKIP sanitizers: this compiler/runtime does not support them', flush=True)
            return []
    return flags


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument('--no-sanitize', action='store_true')
    sanitizer.add_argument('--require-sanitizers', action='store_true')
    parser.add_argument('--require-rtl', action='store_true', help='fail instead of skipping missing Icarus Verilog')
    parser.add_argument('--mbedtls', type=Path, help='Mbed TLS 3.6 source tree, not an installed binary library')
    parser.add_argument('--rom', type=Path, help='optional privately supplied 16 KiB Apple ROM')
    parser.add_argument('--disk-rom', type=Path, help='optional privately supplied 256-byte Disk II ROM')
    parser.add_argument('--disk-image', type=Path, help='optional privately supplied 143360-byte DOS-order disk')
    args = parser.parse_args()
    if any((args.rom, args.disk_rom, args.disk_image)) and not all((args.rom, args.disk_rom, args.disk_image)):
        parser.error('--rom, --disk-rom and --disk-image must be supplied together')
    for name in ('rom', 'disk_rom', 'disk_image', 'mbedtls'):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    BUILD.mkdir(parents=True, exist_ok=True)
    print('Checking release inputs and portable tools', flush=True)
    run([sys.executable, '-m', 'unittest', 'discover', '-s', ROOT / 'tools/tests', '-p', 'test*.py'], timeout=60)
    if (ROOT / '.git').exists():
        run([sys.executable, ROOT / 'tools/check_public.py'])
    else:
        print('SKIP Git source-package guard: this directory is not a standalone Git checkout', flush=True)
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if not compiler or not shutil.which(compiler[0]):
        parser.error('a C compiler is required (set CC if necessary)')
    if not shutil.which('make'):
        parser.error('make is required for storage checks')
    sanitizers = sanitizer_flags(compiler, args.require_sanitizers, args.no_sanitize)
    flags = ['-O1', '-g', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', *sanitizers]
    version = subprocess.run([*compiler, '--version'], capture_output=True, text=True, check=True).stdout
    if 'clang' in version:
        # Recent Clang diagnoses the upstream CPU's intentionally four-byte names.
        warnings = ['-Wno-unknown-warning-option', '-Wno-unterminated-string-initialization']
        supported = subprocess.run([*compiler, '-Werror', *warnings, '-x', 'c', '-c', '-',
                                    '-o', str(BUILD / 'warning_probe.o')],
                                   input='int check;\n', text=True, capture_output=True, timeout=30)
        if supported.returncode == 0:
            flags.extend(warnings)
    includes = ['-Imain', '-Imain/vendor/mii', '-I../storage']
    cpu_flags = ['-DMII_RP2350=0', '-DMII_65C02_DIRECT_ACCESS=1']
    cpu = ['main/apple2.c', 'main/disk2.c', 'main/vendor/mii/mii_65c02.c']

    def c_test(name, sources, extra=(), arguments=(), timeout=30):
        binary = BUILD / name
        run([*compiler, *flags, *includes, *extra, *sources, '-lm', '-o', binary], cwd=MCU)
        run([binary, *arguments], cwd=MCU, timeout=timeout)

    print('Checking portable emulator, controls, audio and SSH terminal', flush=True)
    diagnostic = BUILD / 'diagnostic_image.c'
    run([sys.executable, MCU / 'tools/embed_rom.py', '--output', diagnostic])
    c_test('apple2', [*cpu, str(diagnostic), 'tests/apple2_test.c'], cpu_flags)
    c_test('disk2', ['main/disk2.c', 'tests/disk2_test.c'])
    c_test('video', ['main/video_render.c', 'tests/video_render_test.c'])
    c_test('audio', ['main/audio.c', 'tests/audio_test.c'], ['-DA2_AUDIO_PORTABLE_ONLY'])
    c_test('audio_hook', [*cpu, str(diagnostic), 'tests/audio_hook_test.c'], cpu_flags)
    c_test('handheld_ui', ['main/handheld_ui.c', 'tests/handheld_ui_test.c'])
    source = (MCU / 'main/main.c').read_text()
    begin = source.index('static handheld_ui_event_t sample_physical_buttons(')
    end = source.index('\nstatic void input_task(', begin)
    (BUILD / 'menu_input_impl.inc').write_text(source[begin:end])
    c_test('menu_input', ['main/handheld_ui.c', 'tests/menu_input_test.c'], ['-I' + str(BUILD)])
    c_test('shell_link', ['main/shell_link.c', 'tests/shell_link_test.c'], ['-DSHELL_LINK_PROTOCOL_ONLY'])
    c_test('ssh_terminal', ['main/ssh/tests/ssh_terminal_test.c'], ['-Imain/ssh/tests/stubs'])

    print('Checking storage, NOR flash, journal and DOS catalog', flush=True)
    # Force fresh builds so changing sanitizer/compiler flags cannot reuse old binaries.
    run(['make', '-B', '-C', ROOT / 'storage', 'test-all',
         'BUILD_DIR=build/check', 'CC=' + shlex.join(compiler),
         'SANITIZERS=' + shlex.join(sanitizers)], timeout=120)

    iverilog, vvp = shutil.which('iverilog'), shutil.which('vvp')
    if iverilog and vvp:
        print('Checking FPGA simulations', flush=True)
        rtl = sorted((ROOT / 'fpga/rtl').glob('*.v'))
        model = ROOT / 'fpga/tests/a2_codec_model.v'
        benches = sorted((ROOT / 'fpga/tests').glob('*_tb.v'))
        for bench in benches:
            cases = [('default', [])]
            if bench.stem in ('a2_storage_tb', 'a2_uart_protocol_tb'):
                cases.append(('writes', [f'-P{bench.stem}.FLASH_WRITES=1']))
            if bench.stem == 'a2_uart_protocol_tb':
                cases.append(('hardware_baud', [f'-P{bench.stem}.CLOCK_HZ=33554432',
                                                f'-P{bench.stem}.BAUD=921600',
                                                f'-P{bench.stem}.FLASH_WRITES=1']))
            for label, parameters in cases:
                binary = BUILD / (bench.stem + '-' + label + '.vvp')
                run([iverilog, '-g2012', '-s', bench.stem, *parameters, '-o', binary, *rtl, model, bench])
                run([vvp, binary], timeout=60)
    else:
        if args.require_rtl:
            parser.error('iverilog and vvp are required by --require-rtl')
        print('SKIP FPGA simulations: install iverilog and vvp to enable them', flush=True)

    if args.mbedtls:
        if not (args.mbedtls / 'library/aes.c').is_file():
            parser.error('--mbedtls must point to the Mbed TLS source directory')
        if not shutil.which('ssh'):
            parser.error('OpenSSH client is required for --mbedtls checks')
        print('Checking SSH parser, encryption and localhost OpenSSH interoperability', flush=True)
        command = [sys.executable, MCU / 'main/ssh/tests/test_host.py', '--mbedtls', args.mbedtls]
        if not sanitizers:
            command.append('--no-sanitize')
        run(command, timeout=240)
    else:
        print('SKIP SSH encryption/socket tests: pass --mbedtls /path/to/mbedtls-3.6', flush=True)

    if args.rom:
        if args.rom.stat().st_size != 16384 or args.disk_rom.stat().st_size != 256 or args.disk_image.stat().st_size != 143360:
            parser.error('private inputs require 16384-byte ROM, 256-byte slot ROM and 143360-byte disk')
        print('Checking optional private-ROM BASIC, DOS and game visibility', flush=True)
        c_test('rom', [*cpu, 'tests/rom_probe.c'], cpu_flags, [args.rom])
        with tempfile.TemporaryDirectory(prefix='apple2-rom-check-', dir=BUILD) as temporary:
            disk = Path(temporary) / 'test.dsk'
            shutil.copyfile(args.disk_image, disk)
            c_test('dos', [*cpu, 'tests/dos_probe.c'], cpu_flags,
                   [args.rom, args.disk_rom, disk], timeout=90)
            shutil.copyfile(args.disk_image, disk)
            c_test('basic_game', [*cpu, 'tests/basic_game_test.c'], cpu_flags,
                   [args.rom, args.disk_rom, disk, ROOT / 'basic/BREAKOUT.bas'], timeout=120)
        storage = ROOT / 'storage'
        storage_sources = [storage / name for name in ('a2_storage.c', 'a2_import.c', 'a2_disk_overlay.c',
                          'vendor/littlefs/lfs.c', 'vendor/littlefs/lfs_util.c', 'tests/dos_overlay_test.c')]
        extra = [*cpu_flags, '-DLFS_NO_MALLOC', '-DLFS_NO_DEBUG', '-DLFS_NO_WARN', '-DLFS_NO_ERROR']
        digest = hashlib.sha256(args.disk_image.read_bytes()).hexdigest()
        c_test('dos_overlay', [*cpu, *storage_sources], extra,
               [args.rom, args.disk_rom, args.disk_image, digest], timeout=120)
    else:
        print('SKIP private-ROM/game integration: provide all three explicit ROM/disk flags', flush=True)
    print('PASS source checks (optional skips are listed above)', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit('Check failed: ' + str(error))
