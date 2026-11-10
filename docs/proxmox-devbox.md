# Dev board in a Proxmox container

How the esp-devbox container gets the dev board's USB serial port, so that it can flash and monitor
the board unattended. It works with any ESP32 that is connected through its built-in USB-Serial/JTAG
port (ESP32-S3, C3, C6, P4…).

## The problem

The board shows up on the Proxmox host as `/dev/ttyACMn` (`303a:1001`, "USB JTAG/serial debug unit").
When the board resets while a program still has the port open (esptool does this at the end of every
flash), the host brings it back as the *next free* number. The board then alternates between
`ttyACM0` and `ttyACM1`.

A Proxmox device entry (`dev0: /dev/ttyACM0`) is fixed when the container starts, so it goes stale
after the first such reset. Creating nodes in an unprivileged container's `/dev` from the host doesn't
work either: the host's root isn't mapped in the container, and `mknod` fails with "Value too large
for defined data type".

## The fix

The host keeps the board's current node in a host directory, and the container bind-mounts that
directory. A directory bind is live, so the container always sees the current node.

```
host: udev (ttyACM add/remove) → esp-devbox-sync → /var/lib/esp-devbox/dev/{ttyACMn, board → ttyACMn}
CT:   /dev/esp-dev  (bind mount)               → /dev/esp-dev/board
```

### On the Proxmox host

`/usr/local/sbin/esp-devbox-sync` (mode 755). Set `SERIAL` to the board's USB serial number, which
is its MAC address. It's in `ls /dev/serial/by-id/`.

```sh
#!/bin/sh
# Keep the ESP32 dev board serial node in /var/lib/esp-devbox/dev, which the
# container bind-mounts at /dev/esp-dev. Called by udev.
SERIAL="XX:XX:XX:XX:XX:XX"
D=/var/lib/esp-devbox/dev

mkdir -p "$D"
rm -f "$D"/ttyACM* "$D/board"
for link in /dev/serial/by-id/*"$SERIAL"*; do
    [ -e "$link" ] || continue
    node=$(readlink -f "$link")
    name=$(basename "$node")
    set -- $(stat -c "%t %T" "$node")
    mknod -m 666 "$D/$name" c $((0x$1)) $((0x$2))
    ln -s "$name" "$D/board"
done
exit 0
```

`/etc/udev/rules.d/99-esp-devbox.rules`, then run `udevadm control --reload` and run the script once:

```
SUBSYSTEM=="tty", KERNEL=="ttyACM*", ACTION=="add|remove", RUN+="/usr/local/sbin/esp-devbox-sync"
```

In `/etc/pve/lxc/<ctid>.conf`, add these lines and restart the container. The first line allows
`ttyACM*` (major 166) in the container's device cgroup:

```
lxc.cgroup2.devices.allow: c 166:* rwm
lxc.mount.entry: /var/lib/esp-devbox/dev dev/esp-dev none bind,create=dir 0 0
```

No start hook is needed: the directory lives on the host, so it is already current when the container
starts. Proxmox also rejects raw `lxc.hook.start-host` lines.

### In the container

The port is `/dev/esp-dev/board`, which isn't named `ttyACM*`. esptool therefore can't tell that it's
a USB-Serial/JTAG port, and falls back to the reset sequence for UART chips. That fails with "Wrong
boot mode detected (0x8)". Give it the USB-Serial/JTAG sequence explicitly, in
`~/.config/esptool/esptool.cfg`:

```ini
[esptool]
custom_reset_sequence = R0|D0|W0.1|D1|R0|W0.1|R1|D0|R1|W0.1|D0|R0
```

That is `usb_jtag_bootloader_reset()` from esptool 5.x, with `R`/`D` = RTS/DTR and `0`/`1` = pyserial
`False`/`True`. Then put this at the top of `~/.bashrc`, before Ubuntu's "If not running
interactively" guard, so that `ssh host '<cmd>'` gets it too:

```sh
export PATH="$HOME/.local/bin:$PATH"
export ESPPORT=/dev/esp-dev/board
export ESPTOOL_CFGFILE=$HOME/.config/esptool/esptool.cfg
```

The user needs no group membership: the nodes are created world-readable/writable (`666`), and
they are scoped to this one board by its serial number.

## Checking it

```sh
. ~/esp/esp-idf/export.sh
idf.py flash                                   # no -p needed
for i in 1 2 3 4; do
    esptool --port /dev/esp-dev/board --after watchdog-reset chip-id | grep Connected
    sleep 4; readlink /dev/esp-dev/board       # alternates ttyACM0 / ttyACM1, always connects
done
```
