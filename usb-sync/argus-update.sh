#!/bin/bash
# Install a sensor-payload package that argus-usb-sync copied from a USB stick.
set -euo pipefail

package=/var/cache/argus-update/sensor-payload.deb
[[ $EUID == 0 ]] || { echo "argus-update: Must run as root" >&2; exit 1; }
[[ -f $package ]] || { echo "argus-update: No package queued"; exit 0; }

# Holding the export lock lets a running export finish first and makes new
# exports wait until the install is done.
exec 9>/run/argus-usb-sync.lock
flock 9
trap 'rm -f -- "$package"' EXIT

echo "argus-update: Installing sensor-payload $(dpkg-deb -f "$package" Version)"
# Tells preinst that exports are held off by the lock above.
export ARGUS_UPDATE=1
# Keep configuration edited on the target; dpkg never prompts (see INSTALL_TARGET.md).
dpkg --force-confdef --force-confold -i "$package"
echo "argus-update: Installed sensor-payload $(dpkg-query -W -f='${Version}' sensor-payload)"
