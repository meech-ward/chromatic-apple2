# Installation and recovery

These procedures target the tested GW5A/ESP32 board. Confirm your hardware first.
Use a cartridge whose previous contents you are prepared to replace. Keep your
own raw factory backups before permanently installing the custom FPGA design.

## Identify and back up

Connect the intended powered-on Chromatic to the Mac. The pinned tool suite
includes `openFPGALoader`; `device.py` also accepts `--loader PATH` or a loader on
`PATH`. Its selection guard requires one GWU2X programmer and an explicitly
selected serial. It does not determine the board, LCD or cartridge revision.

```sh
python3 device.py status
mkdir -p build/private
python3 device.py --serial PROGRAMMER_SERIAL \
  backup build/private/factory-fpga.bin --size 4194304
shasum -a 256 build/private/factory-fpga.bin
```

Replace `PROGRAMMER_SERIAL` with the serial from your own status output. The
backup is the complete 4 MiB FPGA configuration flash, not a `.fs` bitstream.
Keep another private copy and its checksum outside the checkout. Backup and flash
detection interrupt any currently loaded SRAM design. Existing backup files are
never overwritten by the helper.

While factory FPGA firmware still exposes its USB serial interface, identify the
Chromatic's ESP32 serial port. Check that it belongs to this device before using
it; another USB serial device is not a substitute. Back up its flash
and inspect its partition table using the activated ESP-IDF Python environment:

```sh
python -m esptool --chip esp32 --port CHROMATIC_PORT \
  read_flash 0 0x400000 build/private/factory-esp32.bin
python -m esptool --chip esp32 --port CHROMATIC_PORT \
  read_flash 0x8000 0x1000 build/private/esp32-partitions.bin
python "$IDF_PATH/components/partition_table/gen_esp32part.py" \
  build/private/esp32-partitions.bin
```

The tested layout has a 1 MiB factory application at `0x10000`, with NVS at
`0x9000`. Confirm that layout and that your application fits before using the
application-only write below. A different layout needs its own installation
plan; do not substitute a generic full-flash command. Raw backups contain private
device data and are not release artifacts.

## Install and test

1. Build the ESP32 application and FPGA design as described in [Building](build.md).
2. With factory USB serial still present, write only the application:

   ```sh
   python -m esptool --chip esp32 --port CHROMATIC_PORT --baud 115200 \
     --before default_reset --after hard_reset \
     write_flash --no-compress --flash_mode keep --flash_size keep \
     --flash_freq keep 0x10000 mcu/build/chromatic_apple2.bin
   ```

3. Check Wi-Fi/SSH access, then load the custom FPGA image into volatile SRAM:

   ```sh
   python3 device.py --serial PROGRAMMER_SERIAL \
     load build/fpga/design.fs --sha256 IMAGE_SHA256
   ```

   Use the image's SHA-256 from its build manifest or `shasum -a 256`. SRAM loading
   does not replace the installed image. Reconfiguration interrupts the running
   machine; sync disk changes before programming.
4. Verify display, physical controls, SSH and the intended cartridge operations.
   [Usage](usage.md) describes initializing dedicated cartridge storage and
   importing a disk. Do not enable cartridge writes on unverified hardware.
5. After those checks, persist the tested FPGA image:

   ```sh
   python3 device.py --serial PROGRAMMER_SERIAL \
     install build/fpga/design.fs --sha256 IMAGE_SHA256
   ```

   Installation writes configuration flash, verifies it and resets the FPGA.
   Check a full power cycle before depending on automatic startup.

## Recover or update the ESP32 later

The custom FPGA currently disables its USB device interface. GWU2X programming
remains available, but the factory USB serial console is absent. The verified
recovery route is restoring your complete raw factory FPGA backup:

```sh
python3 device.py --serial PROGRAMMER_SERIAL \
  install build/private/factory-fpga.bin --sha256 BACKUP_SHA256
```

For `.bin`, the helper explicitly selects external configuration flash, writes
raw bytes at offset zero and verifies them. Restore the backup from this device,
using its recorded checksum.
After factory USB returns, update the ESP32 application, then reinstall the
custom FPGA image. Restoring FPGA flash does not restore the separate ESP32 app.

`device.py restore` only reloads the image currently installed in configuration
flash. It is not a factory-backup restore. Loading a stock `.fs` into SRAM has
not independently established a working USB recovery path; keep the raw backup.
