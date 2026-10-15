# Controls, disks and remote access

**MENU → WI-FI** shows the current network, address and SSH login. Its **TURN
WI-FI ON/OFF** control remembers the choice and keeps your saved network settings.
Use **CHANGE WI-FI / PASSWORD** for one-time setup or later edits; see the
[Wi-Fi guide](wifi.md).

## Handheld controls

| Control | Machine view | Menu / program browser |
| --- | --- | --- |
| Side MENU | Open menu and pause | Close menu and resume |
| D-pad | Apple cursor keys; held state available to games | Move selection; left/right pages the program list |
| A | Game button | Select, run or type the selected on-screen key |
| B | Game button | Back |
| START | Return | — |
| SELECT | Ctrl-C / BASIC Break | — |

MENU → PROGRAMS reads up to 64 Applesoft BASIC names from the selected floppy.
Choose one with up/down and press A to start it. Launch resets the emulated
machine, boots DOS and queues `RUN` after the keyboard prompt is ready. Save an
unsaved program before launching another or choosing Restart. The browser
searches the current disk; it is not a disk-image picker.

Opening a menu pauses emulation. An in-progress disk write finishes before the
CPU pauses or the program list is read. The on-screen keyboard keeps emulation
running so typed characters can be consumed. Physical MENU presses are retained
across delayed polling. Device Info shows the assigned IP and storage state.

The included original [Breakout source](../basic/BREAKOUT.bas) recreates the sideways
Apple II game: held **Up/Down** move the left paddle, **A** serves, and **B**
pauses. Cursor Up/Down or W/S work through SSH. Its BASIC game logic retains
native input/paddle drawing and flicker-free ball updates.
[Game details and historical references](breakout.md) explain the rules and
handheld adaptations. The readable
[helper generator](../tools/generate_breakout_input.py) reproduces its embedded
DATA lines; `python3 tools/generate_breakout_input.py --check basic/BREAKOUT.bas`
checks that they match. BASIC programs can read the held-button mask
with `PEEK(49392)` (`$C0F0`): START=1, SELECT=2, B=4, A=8, UP=16, RIGHT=32,
LEFT=64 and DOWN=128. Menu controls are withheld from the game while visible.

To install Breakout on your own DOS disk, connect through SSH, stop the current
program with Ctrl-C, then enter `TEXT:HOME` and `NEW` at the BASIC prompt. Paste the numbered lines
from `basic/BREAKOUT.bas`, then enter `SAVE BREAKOUT` and wait for the save to
finish. `RUN` starts it; it will also appear in MENU → PROGRAMS. `NEW` discards
the program currently in RAM, so save your work first.

## SSH

```sh
ssh apple@DEVICE_HOST
```

Use the IP shown in Device Info and the `ssh_password` from your private build
configuration. The device generates its SSH host identity and saves it in NVS.
SSH offers the Apple II terminal, not a Unix shell. It supports one connection,
password authentication and an ANSI terminal; file transfer and port forwarding
are not implemented. Graphics remain on the handheld display.

Ctrl+] opens/closes the menu. Arrow keys navigate, Enter selects and Esc goes
back. In machine view, type BASIC normally; Backspace edits and Ctrl-C interrupts
a BASIC loop. Reconnect if the session requests it. USB and Bluetooth keyboards
attached directly to the handheld are unsupported in this release; a keyboard
attached to the SSH computer works normally.

## What is stored where?

| Location | Contents |
| --- | --- |
| ESP32 flash | Firmware, supplied ROMs and private network settings |
| ESP32 NVS | Selected disk, SSH host identity, Wi-Fi profile, setup password and on/off choice |
| ESP32 RAM | 48 KiB Apple memory, running program and disk/display buffers; volatile |
| Cartridge 4 MiB flash | Floppy images, filesystem and persistent SAVE journals |

The cartridge's separate 32 KiB save RAM is not the disk backend. Each floppy
image is 143,360 bytes (140 KiB). Filesystem allocation, changed-sector journals
and compaction need additional room; the 4 MiB capacity is shared by all disks.

Removing a cartridge does not erase a program already loaded into RAM, but the
next disk access may fail. Cached data can briefly hide removal. Seamless live
removal/reinsertion is not implemented. Finish saving, sync/eject, and power off
before swapping cartridges.

## Disk images and saves

Use a bootable DOS-order `.dsk` image you provide. On a compatible dedicated
cartridge, `storage probe` identifies the supported flash. `storage format`
explicitly erases/replaces cartridge storage, including previous contents; a
failed mount never formats automatically.

```sh
python3 client.py --host DEVICE_HOST command 'storage probe'
# Destructive initialization of a dedicated cartridge:
python3 client.py --host DEVICE_HOST command 'storage format'
python3 client.py --host DEVICE_HOST --name Workshop.dsk upload /path/to/Disk.dsk
```

Upload imports and boots the image. Replacing a named disk starts a fresh save
journal for it, so retain your own disk backups. If a connection fails during an
upload, inspect device state before retrying; the client does not retry writes.

At the DOS prompt, a normal BASIC session looks like:

```text
10 PRINT "HELLO, CHROMATIC"
20 END
SAVE MYDEMO
CATALOG
LOAD MYDEMO
RUN
```

Menu Save BASIC/Load BASIC use the default name `PROGRAM`. For another name, type
the DOS command through SSH. Wait for the save and synchronization to complete
before switching off: `storage_saving` must be false and `storage_error` zero,
or explicitly request `storage sync` and check its success.

Saved changes are stored as committed, CRC-checked sector journals bound to the
base disk's SHA-256. Ordinary restart keeps the selected disk and its committed
saves; unsaved RAM contents are not a snapshot. This is standard byte-track Disk
II emulation, without flux-level or copy-protection support.

## Local HTTP control

`client.py` reads `api_token` from `build/device-local.json`, or a file supplied
with `--config`. HTTP uses a token over the local network; interactive SSH is
encrypted. Keep the HTTP interface on a trusted network.

```sh
python3 client.py --host DEVICE_HOST status
python3 client.py --host DEVICE_HOST text
python3 client.py --host DEVICE_HOST command 'a2 type PRINT 2+2\r'
python3 client.py --host DEVICE_HOST command 'storage catalog'
python3 client.py --host DEVICE_HOST command 'storage boot Workshop.dsk'
python3 client.py --host DEVICE_HOST command 'storage sync'
```

The `\r` above is interpreted by the device as Return. Close a paused handheld
menu before using `a2 type`. `storage catalog` lists disk-image files;
`a2 programs` lists BASIC programs inside the active disk. `storage boot NAME`
remembers that disk for the next restart. `storage digest` synchronizes and
hashes the current logical floppy, including saved changes.

| Endpoint | Contract |
| --- | --- |
| `GET /status` | JSON machine, storage, display, audio and input status |
| `GET /text` | Apple II text-screen snapshot |
| `POST /command` | Plain command body, 1–255 bytes, no NUL |
| `POST /disk?name=Workshop.dsk` | Exactly 143,360 disk bytes; safe `.dsk` basename |

Every endpoint requires `X-Device-Token`. Upload names are 5–63 ASCII characters,
start with a letter/digit, and may contain letters, digits, spaces, `_`, `-` and
`.`; paths and `..` are rejected. Status includes observed and acknowledged
physical MENU sequence counters for diagnosing input delivery.
