# Building

Run these commands from the repository root. The tested host is Apple Silicon
macOS; the ESP32 build uses ESP-IDF 5.3. The FPGA scripts use the pinned official
OSS CAD Suite, without the proprietary Gowin compiler or its license service.

## Toolchains

Install Apple's command-line development tools and Python 3. Follow the
[ESP-IDF 5.3 setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32/get-started/)
or keep its checkout beside this repository:

```sh
git clone --recursive --branch v5.3 https://github.com/espressif/esp-idf.git ../esp-idf
../esp-idf/install.sh esp32
. ../esp-idf/export.sh
python3 tools/setup_fpga_tools.py
```

The FPGA installer verifies its pinned download and installs under ignored
`build/tools/`. It supports Apple Silicon macOS. `tools/fpga_build.py --suite PATH`
can use an independently installed compatible suite on another host, but that
route has not been validated here.

## ESP32 application

The default build embeds an original 6502 diagnostic and no Wi-Fi credentials:

```sh
idf.py -C mcu -DFPGA_UART_BAUD=921600 build
```

For BASIC, DOS, HTTP control and SSH, prepare private inputs.
[Wi-Fi setup](wifi.md) explains initial credentials and changing networks after
installation without rebuilding.

```sh
python3 tools/configure.py
mkdir -p build/private
```

The configuration tool prompts for Wi-Fi and SSH settings, generates a random API
token, and writes `build/device-local.json` with mode `0600`. A generated SSH
password is stored in that file. These Wi-Fi settings are first-boot defaults;
a profile saved through **MENU → WI-FI SETUP** takes precedence on later starts.
SSH/API credentials remain build settings. The inert
[example configuration](../examples/device-local.example.json) documents its
fields; replace its placeholders if creating a file manually.

Place your own Apple ROM at `build/private/apple.rom` and the 256-byte Disk II
slot ROM at `build/private/disk2.rom`, then build:

```sh
idf.py -C mcu \
  -DROM_INPUT="$PWD/build/private/apple.rom" \
  -DDISK_ROM_INPUT="$PWD/build/private/disk2.rom" \
  -DPRIVATE_CONFIG="$PWD/build/device-local.json" \
  -DFPGA_UART_BAUD=921600 build
```

Apple ROM inputs may be 8, 12 or 16 KiB; the physically tested BASIC/DOS setup uses
a 16 KiB Apple IIe ROM. Other ROMs are accepted as build inputs without a general
compatibility guarantee. DOS also needs a bootable 143,360-byte DOS-order `.dsk`.
ROMs and disk images are not supplied. Generated firmware contains the supplied
ROMs and credentials: keep provisioned binaries private.

CMake caches these input paths. To return to the public diagnostic, explicitly
clear `ROM_INPUT`, `DISK_ROM_INPUT` and `PRIVATE_CONFIG` or use a fresh build folder.
Output is `mcu/build/chromatic_apple2.bin`. Building does not program hardware.
When reusing an older SDK configuration, set `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048`
to allow normal phone/browser request headers for Wi-Fi setup.

## FPGA controller

Start with cartridge power and flash writes disabled:

```sh
python3 tools/fpga_build.py \
  --top chromatic_a2_top \
  --cst fpga/board/chromatic_evt1_x2.cst \
  --out build/fpga \
  --param CART_MODE=0 --param FLASH_WRITES=0 --param UART_BAUD=921600 \
  fpga/rtl/*.v
```

The result is `build/fpga/design.fs`; `build.json` records inputs, parameters and
its SHA-256. The target is `GW5A-EV25UG256CC1/I0`, open-toolchain family `GW5A-25A`.
The ESP32 and FPGA UART baud settings must match.

`CART_MODE=0` leaves cartridge access disabled. The tested switched-power board
uses `CART_MODE=2`; legacy mode is `1` and has not been physically validated by
this project. After verifying your board revision and cartridge routing, rebuild
with `CART_MODE=2` and `FLASH_WRITES=1` for the tested storage configuration.
Those parameters are hardware choices, not universal Chromatic settings. The
flash driver additionally checks its supported chip IDs and geometry.

Continue with [installation and recovery](install.md). Source builds alone do not
establish electrical compatibility with an untested board or cartridge.
