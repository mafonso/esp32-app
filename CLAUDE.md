# esp32-app

ESP-IDF v6.1 firmware for an ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM): web UI, WebSocket hub,
GitHub-Releases OTA, USB console. Also the boilerplate for new projects, so keep it tidy.

## Build, flash, monitor

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py flash monitor     # Ctrl-] quits monitor
```

- If a build dir predates a change to `sdkconfig.defaults`, delete `sdkconfig` (and rerun the build) so the
  new defaults apply. Missing WebSocket support stops the build with an `#error` saying exactly this.
- The dev board is **esp-dev** (`http://172.16.101.70/`, IoT VLAN). It is plugged into the
  **esp-devbox** container, the only machine that flashes it; see below. There is one board: agents
  working in parallel build only and never flash.
- If after flashing the board sits in the ROM bootloader (no app output), reset it into the app:
  `esptool --port <port> --before no-reset --after watchdog-reset chip-id`
- Never use GPIO19/20, TinyUSB or deep sleep in dev firmware: they break USB-Serial/JTAG, which the
  console and auto-reset flashing depend on.
- Secrets (Wi-Fi password) go only in the git-ignored `sdkconfig.secrets`
  (copy `sdkconfig.secrets.example`). Never put them in `sdkconfig.defaults` or source.
- CI (`.github/workflows/build.yml`) builds every PR and push to `main`. Builds must have no
  warnings from our sources.

## Dev box (esp-devbox)

esp-devbox is a Proxmox LXC container with ESP-IDF v6.1, Claude Code, `gh` and this repo at
`~/esp32-app`; esp-dev's native USB port is plugged into its Proxmox host. Log in as `mafonso`.

- The board is at **`/dev/esp-dev/board`**, a link to its current `ttyACM0`/`ttyACM1`: the board
  re-enumerates on some resets and alternates between the two, and a host udev rule keeps the link
  current. Never hard-code `/dev/ttyACM*`. How it works: [docs/proxmox-devbox.md](docs/proxmox-devbox.md).
- `ESPPORT` (so `idf.py flash` needs no `-p`) and `ESPTOOL_CFGFILE` are exported at the top of
  `~/.bashrc`, so they also apply to `ssh esp-devbox '<cmd>'`. Plain `esptool` ignores `ESPPORT`:
  pass `--port /dev/esp-dev/board`.
- `~/.config/esptool/esptool.cfg` sets esptool's `custom_reset_sequence` to the USB-Serial/JTAG one.
  Inside the container esptool can't recognise the port type, and its default UART sequence fails with
  "Wrong boot mode detected (0x8)".
- The board reports over Wi-Fi too: `curl http://172.16.101.70/api/info`, and Settings → Logs in a
  browser (or `/api/logs`) when nobody is on the USB console. Use the IP: on esp-devbox `esp-dev.local`
  goes through the DNS search domain and resolves to a wrong address. If the IP changes, the console
  `status` command shows the new one.
- Hangs that only the user's devices see (not esp-devbox): check the router's IPS threat log first. It
  blocks clients that repeat `Authorization: Basic admin:…`, which is why the page uses a session token.
- `sdkconfig.secrets` is not in the clone. The board keeps its Wi-Fi credentials in NVS across
  flashes, so it isn't needed unless the flash is erased.

## Releasing

```sh
git tag -a vX.Y.Z -m "One-line notes shown on the board"
git push origin vX.Y.Z
```

`release.yml` builds and publishes `esp32-app.bin` and `version.json`; boards install it from
Settings → Check for updates (or `ota check` / `ota update` on the console). A new image that can't
reach Wi-Fi within 60 s rolls back on its own.

## Layout

| Path | |
|---|---|
| `main/app_main.c` | boot order: settings → Wi-Fi → features init → web → feature routes → console → OTA confirm |
| `main/core/` | shared infrastructure: `wifi`, `settings` (NVS), `auth` (admin password), `logbuf` (log ring buffer), `ota`, `web` (HTTP + static files), `web_settings` (settings/admin API), `web_ws` (WebSocket hub), `console_cmds`, `app_events` |
| `main/features/` | one `.c` per feature, plus the registry `features.c` |
| `main/web/` | frontend, embedded as-is: `index.html`, `app.css`, `app.js` (shell), `settings.js` |
| `main/web/features/` | one ES module per feature UI, loaded automatically |
| `main/web_assets.cmake` | embeds `main/web/**` and generates the file table and `/features.json` |

The frontend is plain ES modules: no build step, no CDN, works offline. Every file under `main/web/`
is served at its path (`main/web/features/led.js` → `/features/led.js`) with `Cache-Control: no-cache`.

## How to add a feature

Copy `main/features/sysinfo.c` and `main/web/features/sysinfo.js`; they use every hook.

1. **C file** `main/features/<name>.c` defining
   ```c
   const feature_t <name>_feature = {
       .name = "<name>",
       .init = <name>_init,                     // drivers, tasks, web_ws_on(), event handlers
       .register_routes = <name>_routes,        // optional
       .register_console = <name>_console,      // optional
   };
   ```
   Any `.c` in `main/features/` is compiled automatically. If you need IDF components that `main`
   doesn't require yet, add them to `REQUIRES` in `main/CMakeLists.txt`, and managed components to
   `main/idf_component.yml`. Put a `<name>.h` next to it only if other code calls into the feature.
2. **Registry**: add one `X(<name>)` line to `FEATURE_LIST` in `main/features/features.c`, in
   alphabetical order.
3. **Routes** (in `register_routes`), always via `web_register_routes()` so the static-file wildcard
   stays last:
   ```c
   static const httpd_uri_t routes[] = {
       {.uri = "/api/<name>", .method = HTTP_GET,  .handler = get_handler},
       {.uri = "/api/<name>", .method = HTTP_POST, .handler = post_handler},
   };
   web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
   ```
   In POST handlers: `if (!web_check_csrf(req)) return ESP_OK;` (add `|| !web_require_admin(req)` from
   `auth.h` for anything that changes configuration), then
   `cJSON *body = web_read_json(req, 512); if (!body) return ESP_OK;`, and reply with
   `web_send_json(req, json)` (takes ownership).
4. **Live data** to browsers: `web_ws_broadcast("<name>", cJSON_data)` (takes ownership, any task,
   never blocks, drops when busy). Skip sampling when `web_ws_client_count() == 0`.
   **From browsers**: `web_ws_on("<name>", cb)` in init; `cb(cJSON *d)` runs on the HTTP task and
   borrows `d`.
5. **Console**: in `register_console`, call `esp_console_cmd_register()`. Commands show in `help`.
6. **Events between features**: add an ID to `app_event_id_t` in `core/app_events.h`, post with
   `app_event_post(APP_EVENT_X, &data, sizeof(data))`, subscribe with
   `esp_event_handler_register(APP_EVENT, APP_EVENT_X, handler, NULL)` in init.
   Example: `button.c` posts `APP_EVENT_BUTTON` and `led.c` cycles its mode on it; neither calls
   the other.
7. **UI** (optional): `main/web/features/<name>.js` is picked up by the build and loaded by the page:
   ```js
   export default function (App) {
     App.card({ id: '<name>', title: 'Name', order: 50, mount(el, App) {
       el.innerHTML = '...';
       App.on('<name>', d => { /* update el */ });       // web_ws_broadcast("<name>", ...)
       App.on('$open', () => App.send('<name>'));       // re-sync after (re)connect
       App.send('<name>');
     } });
   }
   ```
   `App.api(path, {method, body})` does fetch with the CSRF header and JSON in/out, throwing
   `Error(text)` with `.status` on HTTP errors. On a 401 it asks for the admin password in its own
   dialog and retries, so a 401 that reaches you means the user cancelled. It signs in with a session
   token (`X-Admin-Token`), never per-request Basic auth: router IPSes (UniFi etc.) block clients that
   send `Authorization: Basic admin:…` repeatedly. Cards are mounted once and never unmounted. Use the CSS classes in
   `app.css` (`card`, `row`, `msg`, `dl`, `button.primary`, `meter`) and its colour variables.
8. **Settings** (optional): the main page is for using the board; configuration goes on the
   Settings page (cog icon), which has Back/Close and a Save/Cancel bar. Add a section from the
   same module:
   ```js
   App.settings({ id: '<name>', title: 'Name', order: 50,   // built-ins use 10–40
     mount(el, ctx) {                                        // ctx.dirty(true) on every edit
       el.innerHTML = '<input data-k="x">';
       el.querySelector('input').oninput = () => ctx.dirty(true);
     },
     async save() { /* POST the values; throw Error(msg) to keep the page open */ },
     reset() { /* put the inputs back to the stored values */ },
   });
   ```
   Save awaits `save()` of every dirty section, then returns to the main page; Cancel, Back, Close
   and Esc call `reset()` (asking first if anything is unsaved). Read-only sections omit both.

## Conventions

- C: 4-space indent, `static` file-local state with `s_` prefix, `static const char *TAG`, short
  comments that say why. Check every `esp_err_t`; a failing feature `init` is logged and skipped,
  never fatal (the OTA rollback confirmation must still run).
- Don't log from code that a log hook may call (`web_ws_broadcast` never logs for this reason).
- New settings follow the `settings_get_x` / `settings_set_x` pattern in `core/settings.c`
  (NVS namespace `app`).
