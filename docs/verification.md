# Verification record and limits

The following describes the tested development unit, not every Chromatic.
Hardware was an OpenAI DevDay Chromatic with ESP32-U4WDH,
GW5A-EV25UG256CC1/I0, the matched LCD and a 4 MiB ISSI IS29GL032 cartridge.
Testing used externally supplied Apple IIe/slot ROMs and DOS-order disk images;
those inputs and provisioned binaries are private.
The hotspot test used one Mac client with the Chromatic at `192.168.4.1`; joining a phone hotspot
and other Wi-Fi clients were not part of this physical test.

| Evidence | Verified behavior |
| --- | --- |
| Owner observed | Readable LCD output, color animation, Breakout on screen, physical side MENU opening/closing, SSH use |
| Automated on device | BASIC arithmetic/loops/arrays and editing; DOS boot; SAVE, restart, LOAD/RUN and matching logical-disk hashes; browser launch; deferred catalog during SAVE; persistent disk choice and SSH identity |
| Native Mac SSH client | Encrypted login, text display and MENU/PROGRAMS navigation |
| Hotspot hardware | Mac DHCP lease over direct WPA2 Wi-Fi; authenticated HTTP/SSH; rejected API tokens and SSH passwords; BASIC arithmetic, SAVE/LOAD/RUN and temporary-file cleanup; unchanged Breakout source and catalog; SSH and Wi-Fi reconnection with the same host identity |
| Audio hardware diagnostics | Codec configuration readbacks, running clocks, silence baseline and nonzero digital data from BASIC bell; audible speaker output not confirmed by listening |
| Host tests | CPU and real-ROM/DOS regressions, graphics layouts, UI/input timing, resampling, transport parsing, storage persistence/recovery, import boundaries and SSH framing/authentication |
| FPGA simulation | UART framing, cartridge bus sequencing/write gates, removal aborts, display/SPI timing, retained MENU events, codec initialization and I2S transport |

Steady CPU operation targets approximately 1.023 MHz. The changing framebuffer
updates at roughly 7 frames/s over the current SPI link; LCD scan rate is a
different measurement. Cartridge service and heavy remote activity can interrupt
emulation. These observations are workload-dependent, not throughput guarantees.

The CPU uses the MII 65C02 core; cycle-exact NMOS 6502 behavior and undocumented
opcode compatibility are not guaranteed. The emulated machine has 48 KiB main
RAM, 40-column text and lores graphics.
High-resolution graphics are monochrome and scaled to 160×144; NTSC artifact
color is not implemented. Auxiliary RAM, an Integer BASIC language card and a
general card ecosystem are outside the current implementation. The program
browser lists Applesoft BASIC entries on one selected disk, not arbitrary binary
applications. Copy-protected/flux-level disks and standalone USB/Bluetooth
keyboards are unsupported. Cartridge hot-plug recovery and other hardware
revisions have not been established.

## Reproducing checks

The public diagnostic and host checks do not require hardware or Apple ROMs:

```sh
python3 tools/check.py
```

The runner includes portable CPU, graphics, audio, controls, storage and protocol
checks. It uses ASan/UBSan when available and runs FPGA benches when Icarus
Verilog is installed; missing optional dependencies are reported as skipped.
`--require-sanitizers --require-rtl` makes those requirements strict. Supplying
`--mbedtls /path/to/mbedtls-3.6-source` adds encrypted localhost OpenSSH tests with
Clang and an `ssh` client. No check opens or programs physical hardware. Storage's
focused simulations can also run with `make -C storage test test-flash`.

Private BASIC/DOS regression inputs can be supplied separately:

```sh
python3 mcu/tools/test_host.py --sanitize \
  --rom "$PWD/build/private/apple.rom" \
  --disk-rom "$PWD/build/private/disk2.rom" \
  --disk-image /path/to/bootable.dsk
```

The harness modifies an ignored test copy of the disk, not the source image.
Passing host checks does not replace physical display, input or electrical
validation on another unit.
