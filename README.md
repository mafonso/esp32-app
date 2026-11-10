# esp32-app

ESP32-S3 (N16R8) firmware that serves a status page and updates itself over Wi-Fi from this repo's GitHub Releases.

- **Web page** at `http://esp32-xxxxxx.local/` (the last 3 bytes of the board's MAC address, or the name set with `name`): firmware version, board stats, Wi-Fi, and **Check for updates / Install**
- **OTA** from GitHub Releases, with automatic rollback: a new image that can't reach Wi-Fi within 60 s is reverted on the next boot
- **USB setup menu**: connect to the native USB port with any serial terminal to get an `esp32>` prompt

## Build and flash over USB

```sh
. ~/esp/esp-idf/export.sh
cp sdkconfig.secrets.example sdkconfig.secrets   # set your Wi-Fi password
idf.py build flash monitor
```

The Wi-Fi password in `sdkconfig.secrets` (git-ignored) is only used to seed the board's NVS settings storage on first boot.
Release builds made in CI don't contain it, and OTA updates keep the credentials already in NVS.

## USB setup menu

```
idf.py monitor        # or: screen /dev/cu.usbmodem* 115200
esp32> wifi IoT mypassword
esp32> name esp-dev       # DHCP/mDNS hostname, kept across OTA ('name -' resets)
esp32> status
esp32> ota check
esp32> ota update
```

## Releasing a new version

```sh
git tag -a v0.2.0 -m "Short description shown on the board"
git push origin v0.2.0
```

The `Release firmware` workflow builds the firmware and publishes `esp32-app.bin` plus a `version.json` manifest.
Boards read `releases/latest/download/version.json`, and install when you click **Install** (or run `ota update`).

## HTTP API

| Method | Path | |
|---|---|---|
| GET | `/api/info` | firmware, board and Wi-Fi stats |
| GET | `/api/ota` | OTA state and progress |
| POST | `/api/ota/check` | fetch the latest release manifest |
| POST | `/api/ota/update` | install the release found by the last check |
| POST | `/api/reboot` | restart |

POST requests must send an `X-Requested-With` header (any value).

## Layout

| | |
|---|---|
| `partitions.csv` | two 6 MB OTA slots, NVS, 3.9 MB `storage` |
| `main/ota.c` | manifest check, HTTPS OTA, rollback confirmation |
| `main/web.c`, `main/index.html` | HTTP server and status page |
| `main/console_cmds.c` | USB setup menu |
| `main/wifi.c`, `main/settings.c` | station and mDNS, NVS credentials |
