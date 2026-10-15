# Wi-Fi setup

Change the network from your phone or computer. Settings are saved on the
Chromatic and survive power-off; no rebuild or reinstall is needed.

## Change Wi-Fi

1. Open the Chromatic's MENU and choose **WI-FI SETUP**.
2. On your phone or computer, join **CHROMATIC-XXXXXX**, using the setup password
   shown on the handheld. Stay connected when warned that there is no internet.
   The setup password is saved on the device, so your phone can remember it.
3. Open **http://192.168.4.1** in a browser.
4. Choose **Join an existing Wi-Fi network** or **Create a Chromatic hotspot**,
   enter the network name and password, then select **Save Wi-Fi settings**.

For an existing network, the page tests the connection and shows its assigned IP
address. The setup network remains available during this check. After it
connects, press **B** or **MENU** to close setup, reconnect your computer to the
regular network, and use the displayed address for SSH.

For a Chromatic hotspot, the settings take effect when you close setup with
**B** or **MENU**, or restart the device. Join the network you just saved; the
Chromatic's address is **192.168.4.1**. No router is needed. This hotspot provides
local access to the Chromatic, not an internet connection.

Existing networks must provide 2.4 GHz WPA2-Personal Wi-Fi. New passwords must
contain 8–63 printable ASCII characters; station mode also accepts a 64-digit
hexadecimal key. Enterprise authentication and captive-portal sign-in are not
supported. A compatible phone hotspot uses the existing-network option; that
particular arrangement has not been physically tested.

## If the connection fails

After about 25 seconds without a station connection, the Chromatic starts its
protected setup network. Open **MENU → WI-FI SETUP** to see its name and password,
then follow the steps above. Check the exact network name and password before
saving again. Connection attempts pause during recovery so the setup network stays
available. Saving new details or closing setup starts another attempt. You do not
need a factory reset or a firmware rebuild.

Opening setup while using a Chromatic hotspot temporarily replaces that hotspot
with the setup network. Your saved settings remain intact until you select
**Save**. Closing setup restores the selected hotspot.

Guest-network isolation, a VPN, or a second interface using `192.168.4.x` can
prevent local access. For a direct hotspot connection, stay on its network even
though it has no internet.

## SSH and local control

Read the current address in **MENU → Device Info**, then connect:

```sh
ssh apple@DEVICE_IP
```

For a Chromatic hotspot, use `ssh apple@192.168.4.1`. Enter the SSH password from
your private build settings. The SSH password is separate from the Wi-Fi password;
changing Wi-Fi keeps the SSH identity, API token and cartridge programs intact.
One device can join the setup network or hotspot at a time. Only one SSH
session is supported.

For HTTP control, use the installed build's private settings and current address:

```sh
python3 client.py --host DEVICE_IP --config build/device-local.json status
```

The client's token remains valid after a Wi-Fi change. Its configured Wi-Fi name
and password are only initial build defaults, not a copy of the device's current
settings. [Remote access](usage.md#local-http-control) covers commands and disk
import.

## First build

For a new installation, create private bootstrap settings:

```sh
python3 tools/configure.py
```

Or choose a hotspot as the initial mode:

```sh
python3 tools/configure.py --wifi-mode hotspot --output build/hotspot-local.json
```

The tool prompts for Wi-Fi and SSH settings, generates an API token, and writes
an owner-only (`0600`) file. Follow [Building](build.md#esp32-application) and
[installation and recovery](install.md). A diagnostic build without
`PRIVATE_CONFIG` has networking disabled.

The compiled Wi-Fi settings are used until you save a profile through setup.
That profile is stored separately on the device and takes precedence on later
starts. SSH/API credentials still come from the private build settings. Keep
those files and provisioned firmware private: binaries include bootstrap
credentials and any supplied ROMs.

Setup is available only through the temporary protected network, with request
checks and a per-session form token. It is not an unauthenticated settings page
on your home network. See the [verification record](verification.md) for tested
hardware behavior.
