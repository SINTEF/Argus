#!/usr/bin/env bash
# Install a prebuilt payload and its runtime files over SSH.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: install_target.sh [--binary PATH] [--data-dir PATH] [--no-start] USER@HOST

Installs the files in packaging/files.list (the same layout as the Debian
package, without the docs), removes the layout used by earlier versions of this
script, enables the service at boot, and restarts it. It does not check for an
installed sensor-payload package and overwrites its files.
Default binary: build-rpi4-aarch64/SensorPayload (relative to the repository).
--data-dir puts sensor-fields.conf and en_big_screen/ in PATH instead of
/etc/argus. The service passes this directory to SensorPayload.
--no-start installs and enables the service but leaves it stopped.
Requires root or passwordless sudo on the target. SSH aliases are accepted.
EOF
}

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary="$repo_dir/build-rpi4-aarch64/SensorPayload"
start_service=yes
data_dir=
destination=
while (($#)); do
    case "$1" in
        --binary)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            binary=$2; shift 2 ;;
        --data-dir)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            data_dir=${2%/}; shift 2 ;;
        --no-start) start_service=no; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "Unknown option: $1" >&2; exit 2 ;;
        *)
            [[ -z $destination ]] || { usage >&2; exit 2; }
            destination=$1; shift ;;
    esac
done
[[ -n $destination && $destination != *[[:space:]]* ]] || { usage >&2; exit 2; }
# The directory is written into the unit file, so keep it to plain path characters.
if [[ -n $data_dir && ! $data_dir =~ ^/[A-Za-z0-9._/-]+$ ]]; then
    echo "--data-dir must be an absolute path of letters, digits, '.', '_', '-' and '/': $data_dir" >&2
    exit 2
fi
for command in ssh tar install mktemp python3 sed; do
    command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done
[[ -f $binary && -r $binary ]] || { echo "Build or supply a readable binary: $binary" >&2; exit 1; }
cd -- "$repo_dir"
unit_path=/lib/systemd/system/sensor-payload.service

stage=$(mktemp -d)
trap 'rm -rf -- "$stage"' EXIT
# Stage each file under root/ at its target path; manifest lists "mode path".
while read -r mode source target flags; do
    [[ -z $mode || $mode == \#* || " ${flags:-} " == *" deb "* ]] && continue
    [[ $source == @BINARY@ ]] && source=$binary
    [[ -f $source && -r $source ]] || { echo "Missing file: $source" >&2; exit 1; }
    case "$source" in
        *.json) python3 -m json.tool "$source" >/dev/null || { echo "Invalid JSON: $source" >&2; exit 1; } ;;
        *.sh) bash -n "$source" ;;
    esac
    if [[ -n $data_dir && " ${flags:-} " == *" conf "* ]]; then
        target=$data_dir/${target#/etc/argus/}
    fi
    install -D -m "$mode" "$source" "$stage/root$target"
    printf '%s %s\n' "$mode" "$target" >> "$stage/manifest"
done < packaging/files.list
grep -v '^#' packaging/previous-layout > "$stage/previous-layout"
grep -q '^Environment=ARGUS_DATA_DIR=/' "$stage/root$unit_path" ||
    { echo "Expected Environment=ARGUS_DATA_DIR=/... in scripts/sensor-payload.service" >&2; exit 1; }
if [[ -n $data_dir ]]; then
    sed -i "s|^Environment=ARGUS_DATA_DIR=.*|Environment=ARGUS_DATA_DIR=$data_dir|" "$stage/root$unit_path"
fi
printf '%s\n' "$start_service" > "$stage/start-service"
cat > "$stage/install.sh" <<'REMOTE'
#!/usr/bin/env bash
set -euo pipefail
stage=$1
unit=/lib/systemd/system/sensor-payload.service
echo '[Target] Checking privileges and tools'
[[ $(id -u) == 0 ]] || { echo 'Root privileges required' >&2; exit 1; }
for command in systemctl udevadm rsync findmnt flock; do
    command -v "$command" >/dev/null || { echo "Missing command on target: $command" >&2; exit 1; }
done
# Check the service working directory before stopping an existing deployment.
working_dir=$(sed -n 's/^WorkingDirectory=//p' "$stage/root$unit")
[[ $working_dir == /* ]] || { echo 'Expected an absolute WorkingDirectory' >&2; exit 1; }
install -v -d -m 755 "$working_dir" /var/log/argus
# A running USB export restarts the payload when it ends, so stop exports first.
systemctl stop 'argus-usb-sync@*.service'
# Stopping first avoids replacing a running executable and concurrent serial readers.
if systemctl cat sensor-payload.service >/dev/null 2>&1; then
    echo '[Target] Stopping sensor-payload.service'
    systemctl stop sensor-payload.service
fi
echo '[Target] Removing the layout of earlier installs'
if [[ -f /etc/systemd/system/sensor-payload.service ]]; then
    systemctl disable sensor-payload.service
fi
while read -r path; do
    [[ -z $path ]] || rm -fv -- "$path"
done < "$stage/previous-layout"
rm -rfv -- /usr/local/share/sensor-fields.conf /usr/local/share/en_big_screen \
    /usr/share/argus/sensor-fields.conf /usr/share/argus/en_big_screen
if [[ -d /usr/share/argus ]]; then
    rmdir -v --ignore-fail-on-non-empty /usr/share/argus
fi
echo '[Target] Installing files'
while read -r mode path; do
    install -v -D -m "$mode" "$stage/root$path" "$path"
done < "$stage/manifest"
echo '[Target] Reloading systemd and udev, enabling startup at boot'
systemctl daemon-reload
udevadm control --reload-rules
systemctl enable sensor-payload.service
if [[ $(cat "$stage/start-service") == yes ]]; then
    echo '[Target] Starting sensor-payload.service'
    if ! systemctl restart sensor-payload.service; then
        journalctl -u sensor-payload.service -n 60 --no-pager || true
        exit 1
    fi
    systemctl --no-pager --full status sensor-payload.service
else
    echo 'Installed and enabled at boot; service left stopped (--no-start).'
fi
REMOTE
bash -n "$stage/install.sh"

echo "Installing SensorPayload and runtime files on $destination"
echo "Binary: $binary"
echo "Config directory: $(sed -n 's/^Environment=ARGUS_DATA_DIR=//p' "$stage/root$unit_path")"
echo 'Transferring installation bundle over SSH...'
# No user-supplied paths are interpolated into remote shell code. Transfer into
# a private directory, preflight privileges, then install the complete bundle.
tar -C "$stage" -cf - . | ssh -- "$destination" '
set -eu
if [ "$(id -u)" -ne 0 ]; then sudo -n true; fi
stage=$(mktemp -d)
trap '\''rm -rf -- "$stage"'\'' EXIT HUP INT TERM
tar -xf - -C "$stage"
echo "[Target] Transfer complete; installing runtime files"
if [ "$(id -u)" -eq 0 ]; then
    bash "$stage/install.sh" "$stage"
else
    sudo -n bash "$stage/install.sh" "$stage"
fi
'
echo "Installation completed on $destination"
