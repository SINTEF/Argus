# Deployment: M400 and M350

Argus currently runs on a Raspberry Pi 4 attached to a DJI M400 through
USB VCOM. It previously ran on an M350 with the first E-Port development
kit (v1) over a Pi UART. This file describes how to build and set up the
current M400 deployment, and what to change to move back to the M350 and
the v1 kit.

The other docs go into more detail: [README.md](README.md) (build, package,
configuration) and [INSTALL_TARGET.md](INSTALL_TARGET.md) (install paths, USB
gadget mode, service).

## Adaptions

One adaption that must be taken care of, is the specified serial/by-id
used. This is currently hardcoded into the compiled image (sorry). There
is a plan for splitting this out into /etc/argus/ alongside other
relevant fields.

## Overview

| | M400 (current) + E-Port dev kit v2 | M350 + E-Port dev kit v1 |
| --- | --- | --- |
| DJI link on the Pi | USB gadget VCOM, `/dev/ttyGS0` | Pi UART, `/dev/ttyAMA2` |
| `LINUX_UART_DEV1` (`platform/raspberry_pi/hal/hal_uart.h`) | `/dev/ttyGS0` | `/dev/ttyAMA2` |
| `USB_UART_CONNECTED_TO_UAV_VID/PID` (same file) | `0x2CA3:0xF002` (DJI VCOM) | `0x0403:0x6001` (FT232, SDK default) |
| `USER_BAUD_RATE` (`src/dji_sdk_app_info.h`) | `230400` | `230400` |
| `psdk-vcom.sh` in the service | Required (`ExecStartPre`) | Not used; must be removed |
| Pi boot config | `dtoverlay=dwc2,dr_mode=peripheral`, `dwc2` module | A UART overlay that provides `ttyAMA2` |
| Sensor device | FTDI cable, `/dev/serial/by-id/usb-FTDI_TTL232R-3V3_FTENBB2G-if00-port0`, 115200 | `/dev/ttyAMA0`, 9600 |
| Payload SDK | 3.16.0 (`b79fea6`) | Same |

The sensor connection is separate from the aircraft. The switch to the FTDI
cable at 115200 baud came in the same commit as the VCOM switch, but the sensor
does not need to move back to `ttyAMA0` when the aircraft changes. Change it only
if the sensor is wired to the Pi UART again.

## M400: build

On the build host (x86_64 Linux):

```bash
sudo apt install cmake gcc-aarch64-linux-gnu
git clone https://github.com/dji-sdk/Payload-SDK.git      # inside the repo, ignored by git
git -C Payload-SDK checkout b79fea6                         # PSDK 3.16.0, the tested version
cp src/app_info.h.example src/app_info.h && chmod 600 src/app_info.h
# fill in the DJI application credentials in src/app_info.h
```

`src/dji_sdk_config.h` is also not tracked in git; see
[Review notes](#review-notes). It doesn't change the build today.

Build for the RPi4 (aarch64) and make the Debian package:

```bash
RP_HOST= PAYLOAD_SDK_DIR=$PWD/Payload-SDK ./scripts/build_rp.sh
./scripts/build-dpkg.sh            # or --devbuild for a development build
```

The result is `build-rpi4-aarch64/SensorPayload` and
`dist/sensor-payload_<version>_arm64.deb`.

## M400: Raspberry Pi setup

These steps are done once per Pi; see
[INSTALL_TARGET.md](INSTALL_TARGET.md#configure-usb-gadget-mode-on-the-raspberry-pi-4)
for details.

1. In `/boot/firmware/config.txt`, under `[all]`:
   `dtoverlay=dwc2,dr_mode=peripheral`. Remove any `dr_mode=host` or
   `otg_mode=1` setting.
2. `echo dwc2 | sudo tee /etc/modules-load.d/psdk-vcom.conf`, then reboot.
3. Check that `ls /sys/class/udc` lists a controller and that configfs is
   mounted at `/sys/kernel/config`.
4. Connect the Pi's USB-C port to the DJI port with a USB data cable.
5. Connect the sensor's FTDI cable to a Pi USB-A port.

Install the package:

```bash
scp dist/sensor-payload_<version>_arm64.deb rp:/tmp/
ssh rp 'sudo dpkg --force-confdef --force-confold -i /tmp/sensor-payload_<version>_arm64.deb'
```

At start, the service runs `/usr/sbin/psdk-vcom.sh`, which creates the USB gadget
with DJI's VCOM identity (`2ca3:f002`). It then starts `SensorPayload`, which
connects to the aircraft through `/dev/ttyGS0`. Check it with:

```bash
ssh rp 'ls -l /dev/ttyGS0; journalctl -u sensor-payload.service -b -n 100 --no-pager'
```

The startup log line `target baudrate: 230400 - target UART DEV1=/dev/ttyGS0
DEV2=/dev/ttyAMA0` confirms the transport compiled into the binary.

## Moving back to the M350 and the v1 eval kit

The aircraft link is set at compile time, so the M350 needs its own build and
package.

### Code

1. `platform/raspberry_pi/hal/hal_uart.h`: link over the Pi UART.

   ```c
   #define LINUX_UART_DEV1    "/dev/ttyAMA2"
   #define LINUX_UART_DEV2    "/dev/ttyAMA0"

   #define USB_UART_CONNECTED_TO_UAV_VID (0x0403)
   #define USB_UART_CONNECTED_TO_UAV_PID (0x6001)
   ```

   The VID/PID pair is only used to identify the USB serial adapter on E-Port
   2.0 setups (see the comment in the header). `0x0403:0x6001` is the SDK
   default that was in place for the M350.

2. `USER_BAUD_RATE` in `src/dji_sdk_app_info.h` stays at `230400`. It must
   match the UART baud rate configured for the payload on the aircraft side.

3. Only if the sensor is wired to the Pi UART again, change
   `src/dji_sdk_app_info.h`:

   ```c
   #define SENSOR_SERIAL_DEVICE        "/dev/ttyAMA0"
   #define SENSOR_SERIAL_BAUDRATE      B9600 // or what your sensor serial-device supports
   ```

   `sr_open()` in `src/sensor_reader.c` refuses any rate other than `B115200`,
   so change that check as well (`tests/sensor_reader_test.c` uses `B115200`).
   Also change `DEFAULT_DEVICE` in `scripts/sensor_viewer.py`. Note that
   `LINUX_UART_DEV2` is also `/dev/ttyAMA0`; the PSDK only opens it for the
   second HAL UART, which Argus doesn't use on the M350 today. Check this if
   both end up on the same port.

4. `src/main.c` already has an M300/M350 E-Port branch after
   `DjiAircraftInfo_GetBaseInfo()`, but it is empty. No aircraft-specific code
   is needed for the FC subscriptions or the widget.

### Packaging and service

`scripts/sensor-payload.service` runs `ExecStartPre=/usr/sbin/psdk-vcom.sh`.
Without a USB device controller, the script exits with `No USB Device Controller
found` and the service doesn't start. If the controller exists, the script still
creates an unneeded gadget. 

#### M350 adaptions
For the M350, remove the `ExecStartPre` line. You can also take
`scripts/psdk-vcom.sh` out of `packaging/files.list`, but then add
`/usr/sbin/psdk-vcom.sh` to `packaging/previous-layout`. Otherwise a
target upgraded from an M400 package keeps the old file.

Give M350 packages a distinct version (for example `--set-version`) or build
them from a separate branch. A USB field upgrade (`usb-sync/argus-update.sh`)
installs any newer `sensor-payload` package, so an M400 package on the stick
would replace an M350 install.

### Raspberry Pi setup

1. Remove `dtoverlay=dwc2,dr_mode=peripheral` from `config.txt` and delete
   `/etc/modules-load.d/psdk-vcom.conf`.
2. Enable the UART that becomes `/dev/ttyAMA2` with a `dtoverlay=uartN` line in
   `config.txt`. The `ttyAMA` number depends on which UART overlays are
   enabled, and the old M350 `config.txt` isn't in the repository, so check it
   on the Pi with `ls -l /dev/ttyAMA*` after a reboot. Match the overlay's TX/RX
   GPIO pins to the wiring to the kit.
3. If the sensor goes back on `ttyAMA0`, disable the serial console on it
   (`raspi-config`, Interface Options, Serial Port: no login shell, hardware
   enabled).
4. Wire the Pi UART TX/RX/GND to the E-Port development kit v1 UART (3.3 V
   TTL, TX to RX).

After install, the startup log should show `target UART
DEV1=/dev/ttyAMA2`.
