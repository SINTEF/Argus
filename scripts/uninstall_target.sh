#!/usr/bin/env bash
# Remove a payload installed by install_target.sh. Run on the target.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: sudo uninstall_target.sh [--dry-run]

Run this on the target. Stops and disables sensor-payload.service and removes
the files that install_target.sh installs. The config directory is read from the
installed unit (ARGUS_DATA_DIR, default /etc/argus). Files from the layout of
earlier install_target.sh versions (/usr/local, /etc/systemd/system) go too.
Files owned by a Debian package are left alone, as are sensor logs
(/var/log/argus) and the service working directory.
This includes the USB log export (argus-usb-sync).

--dry-run lists what would be removed without changing anything (no root needed).
EOF
}

dry_run=no
while (($#)); do
    case "$1" in
        --dry-run) dry_run=yes; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
[[ $dry_run == yes || $(id -u) == 0 ]] || { echo "Run as root: sudo $0" >&2; exit 1; }
command -v systemctl >/dev/null || { echo 'Missing command: systemctl' >&2; exit 1; }

# Keep in step with packaging/files.list (without "deb" entries) and
# packaging/previous-layout; build-dpkg.sh checks that every path is listed here.
current_layout=(
    /usr/bin/SensorPayload
    /usr/bin/run_sensor_payload.sh
    /usr/bin/sensor_viewer.py
    /usr/sbin/psdk-vcom.sh
    /etc/argus/sensor-fields.conf
    /etc/argus/sensor-fields-strato58.conf
    /etc/argus/en_big_screen/widget_config.json
    /lib/systemd/system/sensor-payload.service
    /usr/sbin/argus-usb-sync
    /lib/systemd/system/argus-usb-sync@.service
    /etc/udev/rules.d/99-argus-usb-sync.rules
    /usr/sbin/argus-update
    /lib/systemd/system/argus-update.service
)
previous_layout=(
    /usr/local/bin/SensorPayload
    /usr/local/bin/run_sensor_payload.sh
    /usr/local/bin/sensor_viewer.py
    /usr/local/sbin/psdk-vcom.sh
    /etc/systemd/system/sensor-payload.service
    /usr/local/sbin/argus-usb-sync
    /etc/systemd/system/argus-usb-sync@.service
)

# Files that belong to a Debian package (sensor-payload) are not ours to remove.
owned() { command -v dpkg >/dev/null && dpkg -S "$1" >/dev/null 2>&1; }
run() {
    if [[ $dry_run == yes ]]; then echo "Would run: $*"; else "$@"; fi
}

# The unit installed by install_target.sh: current location, else the previous one.
unit=
for candidate in /lib/systemd/system/sensor-payload.service /etc/systemd/system/sensor-payload.service; do
    if [[ -f $candidate ]] && ! owned "$candidate"; then
        unit=$candidate
        break
    fi
done

data_dir=/etc/argus
if [[ -n $unit ]]; then
    configured=$(sed -n 's/^Environment=ARGUS_DATA_DIR=//p' "$unit")
    if [[ -n $configured ]]; then
        # Same restriction as install_target.sh --data-dir.
        [[ $configured =~ ^/[A-Za-z0-9._/-]+$ ]] ||
            { echo "Unexpected ARGUS_DATA_DIR in $unit: $configured" >&2; exit 1; }
        data_dir=${configured%/}
    fi
fi
echo "Config directory: $data_dir"

# A USB export restarts the payload when it ends: block new exports, stop
# running ones, then stop the payload.
rule=/etc/udev/rules.d/99-argus-usb-sync.rules
if [[ -e $rule ]] && ! owned "$rule"; then
    echo 'Disabling USB log export'
    run rm -fv -- "$rule"
    run udevadm control --reload-rules
fi
for export_unit in /lib/systemd/system/argus-usb-sync@.service /etc/systemd/system/argus-usb-sync@.service; do
    if [[ -f $export_unit ]] && ! owned "$export_unit"; then
        run systemctl stop 'argus-usb-sync@*.service'
        break
    fi
done
if [[ -n $unit ]]; then
    echo 'Stopping and disabling sensor-payload.service'
    run systemctl stop sensor-payload.service
    run systemctl disable sensor-payload.service
fi

echo 'Removing files installed by install_target.sh'
for path in "${current_layout[@]}" "${previous_layout[@]}"; do
    # Configuration lives in the directory the unit points at (install_target.sh --data-dir).
    [[ $path == /etc/argus/* ]] && path=$data_dir/${path#/etc/argus/}
    if [[ ! -e $path ]]; then
        continue
    elif owned "$path"; then
        echo "Keeping $path (owned by a package)"
    else
        run rm -fv -- "$path"
    fi
done
for dir in "$data_dir/en_big_screen" "$data_dir"; do
    if [[ -d $dir ]] && [[ -z $(ls -A -- "$dir") ]] && ! owned "$dir"; then
        run rmdir -v -- "$dir"
    fi
done

run systemctl daemon-reload
echo 'Kept /var/log/argus and the service working directory'
