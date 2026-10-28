# Install on the Raspberry Pi

There are two ways to install, both with the same files at the same paths
(`packaging/files.list`):

- **Debian package** (main path): build it with `scripts/build-dpkg.sh` and
  install it with `dpkg`.
- **`scripts/install_target.sh`** over SSH: skips the package. It overwrites
  whatever is there and doesn't check for an installed package, so use it on
  development targets.

Manage a target with one of them, not both: the package refuses to install over
files it doesn't own.

## Install with the Debian package

Build the binary, then the package (nothing is installed on the build machine):

```bash
RP_HOST= ./scripts/build_rp.sh           # build only
./scripts/build-dpkg.sh                  # dist/sensor-payload_<version>_arm64.deb
```

Each run increments the last version number, recorded in `debian/changelog` with
the branch, commit and `-dirty` for uncommitted changes to tracked files, and a
short list of the changes since the last release, taken from the commit subjects. Commit
`debian/changelog` afterwards. `--devbuild` makes a development build instead
(`<last>+dev<timestamp>`, used by `watch_builder.sh`): it upgrades the last
release on a target, the next release replaces it, and `debian/changelog` is not
touched. `--set-version X.Y.Z` sets a new middle or first number (for example
`0.1.0`). See [DPKG-ROADMAP.md](DPKG-ROADMAP.md)
for the full design.

Install on the target:

```bash
scp dist/sensor-payload_0.0.1_arm64.deb rp:/tmp/
ssh rp 'sudo dpkg --force-confdef --force-confold -i /tmp/sensor-payload_0.0.1_arm64.deb'
```

The package stops the service before upgrading, then enables and restarts it.
The files in `/etc/argus` are configuration files: edits made on the target are
kept on upgrade (dpkg writes the new version next to them as `*.dpkg-dist` and
prints a warning), they stay on `dpkg -r`, and only `dpkg --purge` removes them.
`/var/log/argus` is never removed. The package refuses to install while files
from `install_target.sh` are present; remove them with `uninstall_target.sh`
first. Remove the package with `sudo dpkg --purge sensor-payload`.

The package includes the USB log export, which can also install a newer package
from a USB stick in the field once `/etc/argus/allow-usb-upgrade` exists; see
[usb-sync/README.md](usb-sync/README.md#updating-from-a-usb-stick).

## Install remotely from the development machine

Build `build-rpi4-aarch64/SensorPayload`, then run:

```bash
./scripts/install_target.sh root@rp-horus
```

The SSH destination may also be a configured SSH alias. Non-root accounts need
passwordless sudo (`sudo -n`). The installer can run from any working directory.
It installs the existing binary; it does not rebuild it.

| Source | Target |
| --- | --- |
| `build-rpi4-aarch64/SensorPayload` | `/usr/bin/SensorPayload` |
| `scripts/run_sensor_payload.sh` | `/usr/bin/run_sensor_payload.sh` |
| `scripts/sensor_viewer.py` | `/usr/bin/sensor_viewer.py` |
| `scripts/psdk-vcom.sh` | `/usr/sbin/psdk-vcom.sh` |
| `config/sensor-fields.conf` | `/etc/argus/sensor-fields.conf` |
| `config/sensor-fields-strato58.conf` | `/etc/argus/sensor-fields-strato58.conf` |
| `src/en_big_screen/widget_config.json` | `/etc/argus/en_big_screen/widget_config.json` |
| `scripts/sensor-payload.service` | `/lib/systemd/system/sensor-payload.service` |
| `usb-sync/argus-usb-sync.sh` | `/usr/sbin/argus-usb-sync` |
| `usb-sync/argus-usb-sync@.service` | `/lib/systemd/system/argus-usb-sync@.service` |
| `usb-sync/99-argus-usb-sync.rules` | `/etc/udev/rules.d/99-argus-usb-sync.rules` |
| `usb-sync/argus-update.sh` | `/usr/sbin/argus-update` |
| `usb-sync/argus-update.service` | `/lib/systemd/system/argus-update.service` |

The table is `packaging/files.list`, which the Debian package uses too.
Use `--data-dir PATH` to install the configuration (`sensor-fields.conf` and
`en_big_screen/`) somewhere other than `/etc/argus`. The installer writes
the directory into the installed unit as `Environment=ARGUS_DATA_DIR=PATH`, and
`SensorPayload` reads its configuration from there.

The `usb-sync/` files are the USB log export and field upgrade; see
[usb-sync/README.md](usb-sync/README.md).

The installer also removes what earlier versions of it and `usb-sync/install.sh`
installed: the binaries and scripts in `/usr/local/bin` and `/usr/local/sbin`,
the units in `/etc/systemd/system`, and the configuration in `/usr/share/argus`
and `/usr/local/share`.

It creates `/var/log/argus` and the service working directory, stops an existing
service before replacing files, installs executable scripts with mode 755 and
configs with mode 644, reloads systemd, enables boot startup, and restarts the
service. Existing runtime configs are overwritten with the repository versions;
sensor logs are preserved. A failed restart prints recent service logs and causes
the installer to fail.

To select another prebuilt binary or install without starting the service:

```bash
./scripts/install_target.sh --binary /path/to/SensorPayload root@rp-horus
./scripts/install_target.sh --no-start root@rp-horus
```

`--no-start` leaves the service stopped but still enables it for the next boot.
The target needs Bash, tar, standard Linux file utilities, and systemd.
Complete the USB gadget boot configuration below before starting the service;
the installer does not edit boot firmware settings or reboot the target.
The compiled sensor config/log paths must match the destinations above.

## Uninstall

`scripts/uninstall_target.sh` runs on the target itself. Copy it over and run it
there:

```bash
scp scripts/uninstall_target.sh root@rp-horus:/tmp/
ssh root@rp-horus
/tmp/uninstall_target.sh --dry-run   # list what would be removed (no root needed)
sudo /tmp/uninstall_target.sh
```

This stops and disables `sensor-payload.service` and removes the files in the
table above, plus the layout of earlier `install_target.sh` versions, reading the
config directory from the installed unit. It removes the config directory only if
it ends up empty. It keeps `/var/log/argus`, the service
working directory, `/etc/argus/allow-usb-upgrade`, and any file owned by a Debian
package. The USB export rule is taken out of effect and running exports are
stopped before the payload is stopped.

## Live sensor terminal viewer

Run `python3 scripts/sensor_viewer.py` to show sensor fields in a table that
refreshes in place. It defaults to
`/dev/serial/by-id/usb-FTDI_TTL232R-3V3_FTENBB2G-if00-port0`, 115200 baud, and
`$ARGUS_SENSOR_FIELDS` if set, else `$ARGUS_DATA_DIR/sensor-fields.conf` (default
`/etc/argus/sensor-fields.conf`), or `config/sensor-fields.conf` from the
repository when that isn't installed.
It uses Python's standard library only. To try it without a sensor, replay a log
with the serial emulator; see
[serial-emulator/README.md](serial-emulator/README.md#use-with-the-sensor-viewer).

Stop the payload service before using the viewer: simultaneous readers of the
same serial device can consume each other's data.

```bash
sudo systemctl stop sensor-payload.service
python3 scripts/sensor_viewer.py
# After exiting the viewer:
sudo systemctl start sensor-payload.service
```

Use `sudo python3` if your user lacks permission to read the serial device.
Press `q` or Ctrl+C to exit; use arrows or Page Up/Down to scroll. Values are
shown as received, with the configured names and types. Unconfigured columns
are named `field_<index>`, empty tokens show `<empty>`, and configured columns
absent from a row show `<missing>`. The table replaces the previous sample.

Override paths with `--device` and `--config`. Use `--plain` to print successive
samples without terminal control, and `--count N` to stop after N samples.
Redirected output automatically uses plain mode.

## Service installation

Run the following commands on the Raspberry Pi from the project directory.

## Prerequisites

Ensure these files and directories are installed before starting the service:

- `/usr/bin/SensorPayload`: executable built for the target architecture.
- `/usr/sbin/psdk-vcom.sh`: VCOM setup script.
- `/etc/argus/sensor-fields.conf`: sensor field mapping. `ARGUS_SENSOR_FIELDS`
  selects another one, such as `/etc/argus/sensor-fields-strato58.conf` (see
  [README.md](README.md#selecting-a-field-mapping)).
- `/etc/argus/en_big_screen/widget_config.json`: widget configuration.

  Both live under `ARGUS_DATA_DIR`, which defaults to `/etc/argus` and is
  set by `Environment=ARGUS_DATA_DIR=` in the service file.
- `/home/pi`: the working directory specified in the service file. If your target
  uses another directory, update `WorkingDirectory` before installing the service.

Create the configured sensor log directory if needed:

```bash
sudo install -d -m 755 /var/log/argus
```

If `SENSOR_SERIAL_LOGDIR` specifies another path, use that directory instead.

## Configure USB gadget mode on the Raspberry Pi 4

Complete this setup before starting the service. `psdk-vcom.sh` creates the USB
serial gadget, but does not configure the Pi's boot firmware or enable its USB
device controller.

On Raspberry Pi OS, edit `/boot/firmware/config.txt` (`/boot/config.txt` on older
images). If your image mounts the boot partition at `/config`, edit its
`config.txt` there instead. Use the boot partition's actual location.

Add the following under an `[all]` section, or update an existing `dwc2` entry:

```ini
[all]
dtoverlay=dwc2,dr_mode=peripheral
```

Remove any conflicting `dtoverlay=dwc2,dr_mode=host` or `otg_mode=1` setting that
applies to this Pi. The Pi 4 uses its USB-C port for gadget mode; connect it to
the DJI interface with a USB data cable.

Ensure the controller driver loads at boot:

```bash
echo dwc2 | sudo tee /etc/modules-load.d/psdk-vcom.conf
```

This setup does not require adding `modules-load` to `cmdline.txt`. If an earlier
gadget setup loads `g_serial`, `g_ether`, or `g_multi` there or in module-loading
configuration, remove those entries so they do not claim the controller. Preserve
other kernel arguments and keep `cmdline.txt` on a single line. The VCOM script
loads `libcomposite` itself.

Reboot to apply the firmware setting, then reconnect and check the controller:

```bash
sudo reboot
```

```bash
ls /sys/class/udc
findmnt -t configfs
```

The first command must list a controller; an empty result leads to the script's
`No USB Device Controller found` error. Configfs must be mounted at
`/sys/kernel/config`, where the script creates `usb_gadget/pi4`. Raspberry Pi OS
normally mounts it automatically. If it is missing, mount it before proceeding
and configure your image to mount it at boot:

```bash
sudo mount -t configfs none /sys/kernel/config
```

See the Raspberry Pi [boot configuration documentation](https://www.raspberrypi.com/documentation/computers/config_txt.html)
and [USB controller overlay options](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/overlays/README),
and the Linux kernel [configfs gadget documentation](https://docs.kernel.org/usb/gadget_configfs.html).

## Install the service and launcher

```bash
sudo install -m 644 scripts/sensor-payload.service /lib/systemd/system/
sudo install -m 755 scripts/run_sensor_payload.sh /usr/bin/
sudo install -m 755 scripts/psdk-vcom.sh /usr/sbin/
```

The service runs `/usr/sbin/psdk-vcom.sh` through `ExecStartPre`. This script
must finish successfully before `/usr/bin/run_sensor_payload.sh` starts the
payload application. The service runs as root and restarts after failures with a
five-second delay.

## Enable startup at boot

Reload systemd, enable the service for future boots, and start it now:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now sensor-payload.service
```

If the service was already running, restart it to apply the updated service file:

```bash
sudo systemctl restart sensor-payload.service
```

## Verify the service

```bash
systemctl is-enabled sensor-payload.service
systemctl status sensor-payload.service
journalctl -u sensor-payload.service -b -n 100 --no-pager
```

Expect `enabled` and `active (running)`. Check the journal for VCOM setup, sensor
configuration, serial-device, or widget initialization errors if startup fails.

After successful VCOM setup, `ls -l /dev/ttyGS0` should show the local serial port.

Follow live logs with:

```bash
journalctl -u sensor-payload.service -f
```

## Verify after a reboot

Reboot the Raspberry Pi:

```bash
sudo reboot
```

Reconnect and confirm that the service started automatically:

```bash
systemctl status sensor-payload.service
journalctl -u sensor-payload.service -b --no-pager
```
