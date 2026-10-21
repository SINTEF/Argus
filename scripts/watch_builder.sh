#!/usr/bin/env bash
# Requires inotify-tools. Rebuild and repackage or deploy after source or runtime-file changes.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

usage() {
    cat <<'EOF'
Usage: watch_builder.sh [USER@HOST [BUILD_ARGS...]]

Without arguments: cross-build, then package with
  scripts/build-dpkg.sh --devbuild (nothing is installed).
With USER@HOST: cross-build and install with scripts/install_target.sh,
  bypassing the .deb. Remaining arguments go to scripts/build.sh (CMake).
EOF
}

case ${1:-} in
    -h|--help) usage; exit 0 ;;
    -*) usage >&2; exit 2 ;;
esac
destination=${1:-}
if (($#)); then shift; fi
export BUILD_DIR="${BUILD_DIR:-build-rpi4-aarch64}"

command -v inotifywait >/dev/null || { echo 'Install inotify-tools to use the watcher.' >&2; exit 1; }
[[ -x ./scripts/build_rp.sh ]] || { echo 'Build script is not executable: scripts/build_rp.sh' >&2; exit 1; }
if [[ -z $destination ]]; then
    [[ -x ./scripts/build-dpkg.sh ]] || { echo 'Packaging script is not executable: scripts/build-dpkg.sh' >&2; exit 1; }
fi

build_and_deploy() {
    if [[ -n $destination ]]; then
        RP_HOST=$destination ./scripts/build_rp.sh "$@"
    else
        RP_HOST= ./scripts/build_rp.sh "$@" &&
            ./scripts/build-dpkg.sh --devbuild --binary "$BUILD_DIR/SensorPayload"
    fi
}

# Include deployed configs, widgets, scripts and service files, not build output.
watch_paths=(src platform scripts config usb-sync cmake CMakeLists.txt)
if [[ -n $destination ]]; then
    echo "Watching source and runtime files; building and installing on $destination (no .deb)"
else
    echo 'Watching source and runtime files; building and packaging with build-dpkg.sh --devbuild'
fi
while inotifywait -q -r -e close_write,create,delete,moved_to,moved_from \
    --exclude '(^|/)(#|\.#|__pycache__(/|$)|dist(/|$))|(~|\.swp|\.pyc)$' "${watch_paths[@]}"; do
    if ! build_and_deploy "$@"; then
        echo 'Build, packaging or installation failed; waiting for the next change.' >&2
    fi
    # Changes during the build are not queued by this simple watcher.
done
