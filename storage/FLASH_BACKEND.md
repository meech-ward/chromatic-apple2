# IS29GL032 cartridge backend

`a2_is29gl032.c` is an original MIT implementation using the manufacturer's
[IS29GL064/032 datasheet, revision A7](https://www.issi.com/WW/pdf/29GL032-064.pdf):
command definitions (6.1), identification (6.2), DQ7/DQ5 polling (7.2), and CFI
geometry (10.1–10.5). No third-party flash driver is included.

The [FlashGBX hardware profile](https://github.com/lesserkuma/FlashGBX/blob/master/FlashGBX/config/fc_DMG_iG_4MB.txt)
identifies the ModRetro IS29GL032 cartridge as using cartridge WR and a banked
ROM window at CPU `4000–7fff`, including bank zero. This is interface evidence;
actual board routing and electrical operation still require hardware validation.
The callback receives literal low command CPU addresses and mapped payload
addresses unchanged; a write gate must explicitly permit both address classes.
The flash chip itself requires 2.7–3.6 V; a reader's cartridge-voltage setting is
not the chip supply specification.

Initialization performs no I/O. Probe uses reset, autoselect, and CFI commands;
it does not program or erase array data. It requires IDs `9d/7e/1d/01`, AMD CFI
command-set 2, and exactly 4 MiB with 64 uniform 64 KiB sectors. Each probe
revokes write permission. `a2_is29gl032_allow_writes` is a separate explicit
step; only then can the provided LittleFS callbacks program or erase.

The driver does not alter WP#/ASP protection. In particular, hardware WP# can
protect the highest sector of the TLET part. Those board/protection settings are
not established by identification alone. Every mutation checks completion and
readback; operation failures revoke identification/write permission. No recovery
write is issued after failure: hardware-reset the cartridge and initialize the
driver again before probing. An ambiguous A0/data transport failure is not safe
to recover by blindly writing a reset byte.

Single changed bytes use byte programming; longer changed runs use the chip's
256-byte write-buffer pages. An optional bounded block callback transfers up to
32 sequential data bytes at a time. Each programmed page and erased sector is
read back. Caller-supplied timeouts bound programming/erase polling, and bus
callbacks must separately bound transport I/O. There is no chip-erase operation,
automatic format, or protection-unlock operation. Physical transport callbacks
are supplied by the application.

Run `make -C storage test-flash` from the repository root for the sanitized host NOR model,
including a LittleFS image persistence round trip.
