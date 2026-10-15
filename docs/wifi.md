# Wi-Fi setup

Choose one network mode when building the ESP32 application:

| Mode | How you connect |
| --- | --- |
| `station` (default) | Chromatic joins your existing 2.4 GHz Wi-Fi network, including a compatible phone hotspot. |
| `hotspot` | Chromatic creates a private WPA2 network. Connect your computer directly to it; no router is needed. |

SSH and HTTP control work locally. Internet access is not required. Hotspot mode
provides access to the Chromatic, not an internet connection. There is no automatic
fallback, simultaneous router/hotspot mode, captive portal or handheld setup screen.

## Configure

For an existing Wi-Fi network:

```sh
python3 tools/configure.py
```

Enter its exact network name and Wi-Fi password, then choose a separate SSH
password or leave it blank to generate one. Use a 2.4 GHz WPA2-Personal network;
enterprise authentication and captive-portal sign-in are not configured. Joining
a phone hotspot uses this same mode; it has not been physically tested.

To create a Chromatic hotspot instead:

```sh
python3 tools/configure.py --wifi-mode hotspot --output build/hotspot-local.json
```

Choose the network name your computer will see. The hotspot password must contain
8–63 printable ASCII characters; leave it blank to generate a random password.
Open hotspots are refused. One computer can connect at a time.

The tool generates an HTTP API token and writes owner-only (`0600`) settings. It
does not print passwords or overwrite an existing file. Read `wifi_password` and
`ssh_password` in that private file when connecting; they are separate passwords.

## Build and install

Follow [Building](build.md#esp32-application), selecting the appropriate file:

```sh
# Existing network:
-DPRIVATE_CONFIG="$PWD/build/device-local.json"

# Chromatic hotspot:
-DPRIVATE_CONFIG="$PWD/build/hotspot-local.json"
```

These are options for the documented build command, not standalone shell commands.
Then follow [installation and recovery](install.md). A default diagnostic build
without `PRIVATE_CONFIG` has networking disabled.

Settings are compiled into the application. Changing mode, network or passwords
requires rebuilding and reinstalling it. Existing configuration files without
`wifi_mode` retain station mode. The [example](../examples/device-local.example.json)
shows the fields; its placeholders cannot be used as credentials.

Keep settings and provisioned firmware private: the binary includes credentials
and supplied ROMs. HTTP control is unencrypted and token-protected; use a trusted
network. SSH encrypts the terminal connection.

## Connect

In station mode, connect your computer to the same reachable network. Open the
Chromatic's MENU → Device Info and read its assigned IP address. DHCP addresses
can change after a restart; station mode reconnects if Wi-Fi drops.

In hotspot mode, join the configured Chromatic network with `wifi_password`.
Accept your computer's “no internet” notice and stay connected. Device Info shows
`192.168.4.1`. Then open the Apple II terminal:

```sh
ssh apple@DEVICE_IP
```

Replace `DEVICE_IP` with the displayed address; for hotspot mode use
`ssh apple@192.168.4.1`. Enter `ssh_password` from the installed configuration.
Only one SSH session is supported.

For HTTP status on the hotspot:

```sh
python3 client.py --host 192.168.4.1 --config build/hotspot-local.json status
```

[Remote access](usage.md#local-http-control) also covers disk import and commands.

## Check the connection

After installing either mode, verify Device Info, SSH login, terminal input and
HTTP status. Disconnect/reconnect the computer, then check that a fresh SSH login
works. In hotspot mode, confirm the computer received a `192.168.4.x` DHCP address.
Host tests cover configuration validation and both network event paths. The
[hardware verification](verification.md) covers a Mac joining the Chromatic
hotspot, SSH, BASIC save/load and Wi-Fi reconnection.

If connecting fails:

- `0.0.0.0` means the network is not ready. Check that the installed build used the
  intended private file and that the SSID/password are exact.
- Station mode needs 2.4 GHz. Guest-network/client isolation can block access even
  when both devices have addresses.
- For a Chromatic hotspot, stay on its network despite the lack of internet. A VPN
  or another interface using `192.168.4.x` may interfere with the local route.
- Close another SSH session before opening a new one. Use the SSH password for
  SSH and the Wi-Fi password to join the hotspot.
