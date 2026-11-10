# esp32-app

ESP32-S3 (N16R8) firmware with a web playground for the board, a settings page, and updates over Wi-Fi from this repo's GitHub Releases.
It is also a boilerplate: features are self-contained files (see [CLAUDE.md](CLAUDE.md#how-to-add-a-feature)).

- **Web UI** at `http://esp32-xxxxxx.local/` (the last 3 bytes of the board's MAC address, or the name set with `name`):
  a playground of live feature cards, and **Settings** (cog icon) with firmware version, board stats, **Check for updates / Install**, Wi-Fi and hostname changes, an optional admin password and a live log viewer
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
esp32> passwd s3cret       # web admin password, user 'admin' ('passwd -' removes it)
esp32> status
esp32> ota check
esp32> ota update
esp32> led breathe #ff8000 20   # RGB LED: off|solid|breathe|blink|rainbow [#rrggbb] [brightness]
esp32> led gpio 38              # if the LED stays dark (DevKitC-1 v1.1 boards)
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
| GET | `/api/sysinfo` | board stats only (example feature) |
| GET, POST | `/api/led` | RGB LED state `{mode, color, brightness, speed, gpio}`; POST any subset |
| GET | `/api/button` | BOOT button `{pressed, count, long_press}` |
| GET | `/api/ota` | OTA state and progress |
| POST | `/api/ota/check` | fetch the latest release manifest |
| POST | `/api/ota/update` | install the release found by the last check |
| POST | `/api/reboot` | restart (admin) |
| GET | `/api/wifi/scan` | nearby networks `[{ssid, rssi, channel, auth}]`, strongest first |
| GET | `/api/settings/wifi` | saved SSID and the last change `{ssid, state: idle\|trying\|ok\|failed, attempt, message}` |
| POST | `/api/settings/wifi` | `{ssid, password}` (admin): 202, then tries it for 30 s; saved only if it connects, else the board goes back |
| GET | `/api/settings/name` | `{name, default}` |
| POST | `/api/settings/name` | `{name}` (admin; empty resets): saves and reboots |
| GET | `/api/settings/password` | `{enabled, user}` |
| POST | `/api/settings/password` | `{password}` (admin; empty removes it) |
| GET | `/api/logs?after=<seq>` | (admin) buffered log lines after `seq`: `{seq, boot, lines, dropped}` |

POST requests must send an `X-Requested-With` header (any value).
Routes marked admin need HTTP Basic auth (user `admin`) once a password is set with `passwd` or in Settings; with none set they are open.
OTA check and update are admin routes too.

`/ws` is a WebSocket carrying JSON messages `{"t": topic, "d": data}` both ways; e.g. topic `sysinfo` streams board stats every 2 s, `led` keeps the LED controls of every open page in sync, and `button` reports BOOT presses.
`/features.json` lists the feature UI modules; every other GET serves a file from `main/web/`.

## Layout

| | |
|---|---|
| `partitions.csv` | two 6 MB OTA slots, NVS, 3.9 MB `storage` |
| `main/app_main.c` | boot sequence |
| `main/core/` | Wi-Fi + mDNS, NVS settings, admin auth, log buffer, OTA, HTTP server, settings API and WebSocket hub, USB console, app events |
| `main/features/` | one file per feature, registered in `features.c` (`sysinfo.c` is the example) |
| `main/web/` | frontend shell (`index.html`, `app.js`, `app.css`) and the built-in Settings sections (`settings.js`) |
| `main/web/features/` | one ES module per feature card, listed automatically in `/features.json` |
| `main/web_assets.cmake` | embeds everything under `main/web/` into the firmware |
| `docs/proxmox-devbox.md` | running the dev board from a Proxmox container (USB passthrough that survives resets) |
