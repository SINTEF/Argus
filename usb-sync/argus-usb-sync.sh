#!/bin/bash
set -euo pipefail

fail() {
    echo "argus-usb-sync: $*" >&2
    exit 1
}

[[ $# == 1 ]] || fail "Usage: $0 /dev/DEVICE"
[[ $EUID == 0 ]] || fail "Must run as root"
device=$(readlink -f -- "$1")
[[ -b "$device" ]] || fail "Not a block device: $device"
echo "argus-usb-sync: Starting export for $device"
[[ -d /var/log/argus ]] || fail "Missing source directory: /var/log/argus"

# Serialize service and manual invocations, including different device aliases.
exec 9>/run/argus-usb-sync.lock
flock 9

# Reuse an automounter's mount; only unmount filesystems mounted by this script.
temporary_dir=false
if mount_dir=$(findmnt --noheadings --raw --first-only --source "$device" --output TARGET); then
    # findmnt --raw hex-escapes whitespace and backslashes in paths.
    printf -v mount_dir '%b' "$mount_dir"
    echo "argus-usb-sync: Using existing mount $mount_dir"
else
    mount_dir=$(mktemp -d /run/argus-usb-sync.XXXXXX)
    temporary_dir=true
fi
mounted=false
copy_stage=
restart_payload=false
restore_payload() {
    if [[ $restart_payload == true ]]; then
        echo "argus-usb-sync: Starting sensor-payload.service"
        systemctl start sensor-payload.service || return $?
        restart_payload=false
    fi
}
cleanup() {
    local status=$?
    trap - EXIT
    # Restore the producer even after a copy, flush or unmount failure.
    if ! restore_payload; then
        echo "argus-usb-sync: Could not restart sensor-payload.service" >&2
        status=1
    fi
    if [[ -n $copy_stage ]]; then
        rm -rf -- "$copy_stage" || status=1
    fi
    if [[ $mounted == true ]]; then
        if ! umount -- "$mount_dir"; then
            echo "argus-usb-sync: Could not unmount $mount_dir" >&2
            status=1
        else
            mounted=false
        fi
    fi
    if [[ $temporary_dir == true && $mounted == false ]]; then
        rmdir -- "$mount_dir" || status=1
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Field upgrade: a newer sensor-payload package in the stick's root is copied to
# the target and installed by argus-update.service once this export has ended.
upgrade_flag=/etc/argus/allow-usb-upgrade
queued_package=/var/cache/argus-update/sensor-payload.deb
upgrade_queued=false
queue_upgrade() {
    local candidates deb package architecture version best= best_version= installed=
    shopt -s nullglob
    candidates=("$mount_dir"/sensor-payload_*_arm64.deb)
    shopt -u nullglob
    ((${#candidates[@]})) || return 0
    if [[ ! -e $upgrade_flag ]]; then
        echo "argus-usb-sync: USB upgrade disabled ($upgrade_flag missing); ignoring packages on the stick"
        return 0
    fi
    for deb in "${candidates[@]}"; do
        package=$(dpkg-deb -f "$deb" Package 2>/dev/null) || package=
        architecture=$(dpkg-deb -f "$deb" Architecture 2>/dev/null) || architecture=
        version=$(dpkg-deb -f "$deb" Version 2>/dev/null) || version=
        if [[ $package != sensor-payload || $architecture != arm64 || -z $version ]]; then
            echo "argus-usb-sync: Ignoring ${deb##*/}: not a sensor-payload arm64 package"
            continue
        fi
        if [[ -z $best ]] || dpkg --compare-versions "$version" gt "$best_version"; then
            best=$deb
            best_version=$version
        fi
    done
    [[ -n $best ]] || return 0
    if [[ $(dpkg-query -W -f='${db:Status-Status}' sensor-payload 2>/dev/null) == installed ]]; then
        installed=$(dpkg-query -W -f='${Version}' sensor-payload)
        if ! dpkg --compare-versions "$best_version" gt "$installed"; then
            echo "argus-usb-sync: ${best##*/} is not newer than installed $installed; not upgrading"
            return 0
        fi
    fi
    install -d -m 0700 "${queued_package%/*}" || return 1
    cp -- "$best" "$queued_package.tmp" || return 1
    mv -f -- "$queued_package.tmp" "$queued_package" || return 1
    echo "argus-usb-sync: Queued sensor-payload upgrade ${installed:-none} -> $best_version"
    upgrade_queued=true
}

if [[ $temporary_dir == true ]]; then
    # Let mount detect the filesystem (including FAT32 and exFAT).
    mount -o rw,nosuid,nodev,noexec -- "$device" "$mount_dir"
    mounted=true
fi
mkdir -p -- "$mount_dir/argus"

# Stop only after mounting succeeds, so an unusable device causes no downtime.
if systemctl is-active --quiet sensor-payload.service; then
    # Set before stopping so cleanup also restores it if stop is interrupted.
    restart_payload=true
    echo "argus-usb-sync: Stopping sensor-payload.service"
    systemctl stop sensor-payload.service
fi

# Build a view with USB-safe names, using links so log data is not duplicated.
# Complete this before rsync to reject ambiguous names without partial exports.
copy_stage=$(mktemp -d /run/argus-usb-sync-files.XXXXXX)
mkdir -- "$copy_stage/tree"
find -L /var/log/argus -mindepth 1 \( -type d -o -type f \) -print0 > "$copy_stage/paths"
while IFS= read -r -d '' source_path; do
    relative_path=${source_path#/var/log/argus/}
    usb_path=${relative_path//:/_}
    staged_path="$copy_stage/tree/$usb_path"
    if [[ -e $staged_path || -L $staged_path ]]; then
        fail "Filename collision after replacing colons: $relative_path -> $usb_path"
    fi
    if [[ -d $source_path ]]; then
        mkdir -- "$staged_path"
    else
        ln -s -- "$source_path" "$staged_path"
    fi
    if [[ $relative_path != "$usb_path" ]]; then
        printf 'argus-usb-sync: Export name %q -> %q\n' "$relative_path" "$usb_path"
    fi
done < "$copy_stage/paths"

# Dereference the staged links and keep rsync's temporary-file + rename behavior.
rsync --recursive --copy-links --ignore-existing --whole-file --verbose \
    --itemize-changes --out-format='rsync: %i %n%L (%b bytes transferred)' \
    -- "$copy_stage/tree/" "$mount_dir/argus/"
restore_payload
sync -f "$mount_dir"
# An upgrade problem is reported but does not fail the log export.
if ! queue_upgrade; then
    echo "argus-usb-sync: Could not queue the package upgrade" >&2
    rm -f -- "$queued_package.tmp"
fi
if [[ $mounted == true ]]; then
    umount -- "$mount_dir"
    mounted=false
    echo "argus-usb-sync: Finished copying to $device; filesystem unmounted"
else
    echo "argus-usb-sync: Finished copying to $device; existing mount left mounted"
fi
if [[ $upgrade_queued == true ]]; then
    # A separate unit: this one stops when the stick is pulled, which must not
    # interrupt dpkg. argus-update waits for this export's lock.
    echo "argus-usb-sync: Starting argus-update.service; the stick can be removed"
    systemctl start --no-block argus-update.service ||
        echo "argus-usb-sync: Could not start argus-update.service" >&2
fi
