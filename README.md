# Argus

Argus is a standalone DJI Payload SDK application for a Raspberry Pi payload
computer and a future expansion to Manifold3. It reads CSV sensor rows
from a serial device, merges selected DJI flight-controller state, logs
the combined payload, and updates a small DJI Pilot widget display.

The application binary is `SensorPayload`, and it ships as the `sensor-payload`
Debian package and service.

The project is currently in a PoC stage.

## Install The Debian Package

The payload ships as one Debian package, `sensor-payload_<version>_arm64.deb`,
with the application, its service, the configuration in `/etc/argus` and the USB
log export. Get a package (see [Build The Debian Package](#build-the-debian-package)
below), then install it on the Raspberry Pi:

```bash
scp sensor-payload_0.0.1_arm64.deb rp:/tmp/
ssh rp 'sudo dpkg --force-confdef --force-confold -i /tmp/sensor-payload_0.0.1_arm64.deb'
```

The package enables and starts `sensor-payload.service`. Check it with:

```bash
ssh rp 'systemctl status sensor-payload; dpkg -s sensor-payload | grep Version'
```

Before the first start, set up USB gadget mode on the Pi; see
[INSTALL_TARGET.md](INSTALL_TARGET.md#configure-usb-gadget-mode-on-the-raspberry-pi-4).
Upgrades use the same `dpkg` command. Configuration edited in `/etc/argus` is kept
on upgrade, and sensor logs in `/var/log/argus` are never removed. Remove the
package with `sudo dpkg --purge sensor-payload`.

In the field, a newer package can also be installed from a USB stick once
`/etc/argus/allow-usb-upgrade` exists on the Pi; see
[usb-sync/README.md](usb-sync/README.md#updating-from-a-usb-stick).

The package refuses to install over a target set up with `install_target.sh`
([Development Installs](#development-installs)); clean such a target with
`scripts/uninstall_target.sh` first. [INSTALL_TARGET.md](INSTALL_TARGET.md) has
the details of both install paths.

## Build Locally

Install build tools on the host:

```bash
sudo apt install cmake gcc-aarch64-linux-gnu
```

Download the DJI PSDK repository:

``` bash
cd /opt/
git clone https://github.com/dji-sdk/Payload-SDK.git
```

Create `src/app_info.h` with your DJI application credentials. It isn't tracked
in git, and the build stops without it (see [Configuration](#configuration)):

```bash
cp src/app_info.h.example src/app_info.h
chmod 600 src/app_info.h
```

Build for Raspberry Pi 4 aarch64 without installing (`RP_HOST=` set but empty):

```bash
RP_HOST= PAYLOAD_SDK_DIR=/opt/Payload-SDK ./scripts/build_rp.sh
```

The binary is `build-rpi4-aarch64/SensorPayload`. An existing build directory
keeps its CMake cache; after the move of the configuration to `/etc/argus`,
reconfigure it once with `-DARGUS_DATA_DIR=/etc/argus` (arguments to
`build_rp.sh` go to CMake). `BUILD_DIR=...` selects another build directory.

Host build (for example to run the binary locally), no cross compiler needed:

```bash
PAYLOAD_SDK_DIR=../Payload-SDK ./scripts/build.sh
```

## Build The Debian Package

After a local build, package it (nothing is installed on the build machine):

```bash
./scripts/build-dpkg.sh                  # release: next version, e.g. 0.0.6
./scripts/build-dpkg.sh --devbuild       # development build, e.g. 0.0.5+dev20261001141500
./scripts/build-dpkg.sh --set-version 0.1.0
```

The package is written to `dist/`. Its contents and install paths are listed in
`packaging/files.list`.

- **Release:** increments the last version number and records it in
  `debian/changelog` with the branch, the commit and `-dirty` for uncommitted
  changes to tracked files. Commit the code first, then commit
  `debian/changelog` after the build.
- **`--devbuild`:** `<last release>+dev<UTC timestamp>`, newer than the last
  release and older than the next one, so it upgrades a target and is replaced by
  the next release. `debian/changelog` is not touched, and only the newest
  development build is kept in `dist/`.
- **`--set-version X.Y.Z`:** the manual step for the middle or first number.

See [DPKG-ROADMAP.md](DPKG-ROADMAP.md) for the full design.

## Development Installs

For quick iterations on a development Pi, `scripts/install_target.sh` installs
the same files as the package directly over SSH, without a package. It overwrites
whatever is there and doesn't check for an installed package, so don't mix it
with the package on one target. The SSH account needs root or passwordless sudo.

```bash
RP_HOST=root@rp-horus ./scripts/build_rp.sh     # build, then install_target.sh
./scripts/install_target.sh root@rp-horus       # install the existing build
```

`build_rp.sh` defaults to `RP_HOST=rp`; `RP_HOST=` builds without installing.
To remove such an install, copy `scripts/uninstall_target.sh` to the Pi and run
it there (see [INSTALL_TARGET.md](INSTALL_TARGET.md#uninstall)).

To rebuild whenever source, config, widget, or service files change:

```bash
./scripts/watch_builder.sh                  # build, then package: build-dpkg.sh --devbuild
./scripts/watch_builder.sh root@rp-horus    # build, then install_target.sh (no .deb)
```

Requires `inotify-tools`. The root `./watch_builder.sh` is an equivalent entry
point. Without a destination, the watcher cross-builds and makes a development
package in `dist/`, but installs nothing. With one, it runs `build_rp.sh` and
`install_target.sh` instead. Arguments after the destination go to
`scripts/build.sh` (CMake). The watcher waits for a change before its first build
and remains running after build/install failures. Changes made during a build are
not queued.

## Run Manually

On the Raspberry Pi:

```bash
sudo /usr/bin/SensorPayload "$(date +%F_%H_%M_%S)"
```

The optional argument is appended to the sensor log name by `src/main.c`.

Watch logs:

```bash
journalctl -f
```

Check the configured sensor serial device before running:

```bash
ls -l /dev/ttyAMA0 /dev/ttyS4 /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

## Run At Boot

The package and `install_target.sh` both install and enable
`/lib/systemd/system/sensor-payload.service` (from
`scripts/sensor-payload.service`), so the payload starts at boot. Don't create a
copy in `/etc/systemd/system`: it would override the installed unit, and the
package refuses to install while one exists. To change settings such as
`ARGUS_SENSOR_FIELDS`, use a drop-in (`sudo systemctl edit sensor-payload`; see
[Selecting a field mapping](#selecting-a-field-mapping)).

Inspect it:

```bash
systemctl status sensor-payload.service
journalctl -u sensor-payload.service -f
```

## Configuration

The DJI application credentials go in `src/app_info.h`, which is **not tracked in
git** (it's in `.gitignore`). Create it from the template before the first build;
CMake stops with an error while it's missing:

```bash
cp src/app_info.h.example src/app_info.h
chmod 600 src/app_info.h
# fill in USER_APP_NAME, USER_APP_ID, USER_APP_KEY, USER_APP_LICENSE and
# USER_DEVELOPER_ACCOUNT from https://developer.dji.com/user/apps/#all
```

SensorPayload refuses to start while the template's placeholder values are in
place. Everything else is in [src/dji_sdk_app_info.h](src/dji_sdk_app_info.h),
which includes `app_info.h`:

- `USER_BAUD_RATE`
- `SP_SerialNumber`
- `SENSOR_SERIAL_DEVICE`
- `SENSOR_SERIAL_BAUDRATE`
- `SENSOR_SERIAL_LOGDIR`
- `ARGUS_DATA_DIR` (see [Configuration directory](#configuration-directory))

Current sensor defaults:

```c
#define SENSOR_SERIAL_DEVICE        "/dev/ttyAMA0"
#define SENSOR_SERIAL_BAUDRATE      B9600
#define SENSOR_SERIAL_LOGDIR        "/var/log/argus/"
```

Edit [src/dji_sdk_config.h](src/dji_sdk_config.h) for PSDK feature toggles.

### Configuration directory

The sensor field mapping and the widget UI live in one directory on the target:

- `$ARGUS_DATA_DIR/sensor-fields.conf`
- `$ARGUS_DATA_DIR/en_big_screen/` (contains `widget_config.json`)

The directory is `/etc/argus` by default. To change it:

- At run time: set the `ARGUS_DATA_DIR` environment variable. The service sets it
  with `Environment=ARGUS_DATA_DIR=...` in `scripts/sensor-payload.service`, and
  `install_target.sh --data-dir PATH` rewrites that line during installation.
- At build time: `./scripts/build.sh -DARGUS_DATA_DIR=PATH` changes the default
  used when the variable is unset (for example when running the binary by hand).

The binary logs `Using config directory ..., sensor fields ...` at startup.

### Selecting a field mapping

`ARGUS_SENSOR_FIELDS` selects another field mapping by full path; without it the
payload reads `$ARGUS_DATA_DIR/sensor-fields.conf` (`/etc/argus/sensor-fields.conf`).
Two mappings are installed:

- `/etc/argus/sensor-fields.conf`: the live serial stream (default).
- `/etc/argus/sensor-fields-strato58.conf`: the 58-column Strato log format, as
  in `serial-emulator/Strato100363_260926_160307Eng.txt`. It has no CO2 column, and
  only direction and speed of its weather block are mapped (the header labels
  there don't match the values; see the comments in the file).

To use the Strato mapping for the service, add a drop-in with
`sudo systemctl edit sensor-payload`:

```ini
[Service]
Environment=ARGUS_SENSOR_FIELDS=/etc/argus/sensor-fields-strato58.conf
```

then `sudo systemctl restart sensor-payload`. The drop-in survives package
upgrades. `sensor_viewer.py` honours `ARGUS_SENSOR_FIELDS` too.

### Sensor field mapping

`main()` passes `$ARGUS_DATA_DIR/sensor-fields.conf` to `sr_open()`, which reads
the mapping at startup. Editing the mapping only needs a restart.

Each non-comment line contains a zero-based column index, an `sr_payload` sensor
member name, and its type:

```text
6 ch4_ppm double
7 h2o_ppm double
8 c2h6_ppb double
```

Types must match the members in `src/sensor_reader.h`: `string` for `time_stamp`,
`int` for `inlet_number`, `int64` for `gps_time`, and `double` for all other sensor
members. DJI members are filled separately by the application. Blank lines and
`#` comments are allowed. Duplicate columns/members, unknown members, mismatched
types, and malformed entries cause startup to fail with a config error, as does
a missing config file. Restart the application after editing the config.

The supplied mapping uses columns 0–8 of the new stream, retaining the presumed
timestamp/inlet/pressure/temperature meanings of columns 0–5. Columns 9 onward
are ignored, including GPS columns 15/16 whose order is not yet confirmed.
All payload members are reset to zero for each sample before configured tokens
are converted. Missing columns, empty tokens, `No Data`, invalid numbers, and
numeric overflow remain zero. Extra columns are ignored. Empty tokens retain
their column positions; changing field order requires updating the config.

Input is plain comma-separated data without CSV quoting or a header. Only
newline-terminated rows are parsed (CRLF is accepted). Rows exceeding 4095 bytes
are discarded through the next newline; incomplete rows at EOF are rejected.
Timestamps longer than 63 bytes remain empty. The output log retains the full
`sr_payload` column layout, with zeroes for unmapped sensor values.

Run standalone reader tests without the DJI SDK:

```bash
cc -std=gnu99 -Wall -Wextra -Werror -Itests/stubs \
  tests/sensor_reader_test.c -o /tmp/sensor_reader_test -lutil -lm
/tmp/sensor_reader_test
```

## Runtime Flow

`src/main.c` currently:

1. registers PSDK OSAL, UART HAL, filesystem, and logger handlers
2. opens the sensor serial device and sensor log
3. fills `T_DjiUserInfo`
4. initializes DJI Core
5. initializes the project-owned widget
6. starts the PSDK application
7. subscribes to DJI flight-controller topics
8. reads and parses sensor CSV rows
9. logs sensor + aircraft data
10. updates CH4/CO2 widget state

The sensor input is line-oriented CSV, mapped through
[config/sensor-fields.conf](config/sensor-fields.conf). The older 54-column
[serial-emulator/serial-stream.csv](serial-emulator/serial-stream.csv) needs a
matching mapping before replay; the supplied config targets the new stream.

## Sensor Replay

Replay a saved log to a local device:

```bash
./scripts/replay_sensor_log.sh serial-emulator/serial-stream.csv /dev/ttyS4
```

The script skips the CSV header and writes one row per second by default.

For virtual serial testing, see [serial-emulator/README.md](serial-emulator/README.md).

## Related Docs

- [WIDGET.md](WIDGET.md): custom PSDK widget notes
- [RP-WIRELESS.md](RP-WIRELESS.md): Raspberry Pi Wi-Fi and split-route setup
- [MIGRATED_SETTINGS.md](MIGRATED_SETTINGS.md): migration notes

## Troubleshooting

If CMake cannot find PSDK:

```bash
PAYLOAD_SDK_DIR=/absolute/path/to/Payload-SDK ./scripts/build.sh
```

If the cross compiler is missing:

```bash
sudo apt install gcc-aarch64-linux-gnu
```

If DJI Pilot does not show the widget, verify that `/etc/argus/en_big_screen`
(or `$ARGUS_DATA_DIR/en_big_screen`)
exists on the target and contains a valid `widget_config.json`.
