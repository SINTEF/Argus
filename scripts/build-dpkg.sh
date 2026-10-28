#!/usr/bin/env bash
# Build the sensor-payload Debian package from packaging/files.list.
set -euo pipefail
umask 022

usage() {
    cat <<'EOF'
Usage: build-dpkg.sh [--devbuild] [--set-version X.Y.Z] [--binary PATH]

Builds dist/sensor-payload_<version>_arm64.deb. Nothing is installed.
The version is the top entry of debian/changelog with the last number
incremented (0.0.1 when debian/changelog does not exist yet). The new entry
records the branch, the commit and -dirty for changes to tracked files, and
the subjects of the newest commits since the last release (leaving out commits
that only change debian/changelog or *.md). It is written to debian/changelog
once the package is built.
--devbuild          Development build: <last>+dev<UTC timestamp>, newer than the
                    last release and older than the next one. debian/changelog
                    is not touched, and older development builds in dist/ are
                    deleted. Used by watch_builder.sh.
--set-version X.Y.Z Build this version (must be newer than the last one).
--binary PATH       Default: build-rpi4-aarch64/SensorPayload.
The binary is not built; run scripts/build_rp.sh (RP_HOST= to skip installing).
EOF
}

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary="$repo_dir/build-rpi4-aarch64/SensorPayload"
devbuild=no
set_version=
while (($#)); do
    case "$1" in
        --devbuild) devbuild=yes; shift ;;
        --set-version)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            set_version=$2; shift 2 ;;
        --binary)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            binary=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
binary=$(realpath -m -- "$binary")
for command in dpkg-deb dpkg file python3 install git gzip sed md5sum; do
    command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done
if [[ -n $set_version ]]; then
    [[ $devbuild == no ]] || { echo '--set-version and --devbuild are exclusive' >&2; exit 2; }
    [[ $set_version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Expected X.Y.Z: $set_version" >&2; exit 2; }
fi
cd -- "$repo_dir"
packaging=packaging
changelog=debian/changelog

# Version: from the top changelog entry, unless this is the first package.
last=
if [[ -f $changelog ]]; then
    last=$(sed -n '1s/^sensor-payload (\([0-9]\{1,\}\.[0-9]\{1,\}\.[0-9]\{1,\}\)) .*/\1/p' "$changelog")
    [[ -n $last ]] || { echo "Cannot parse the version on the first line of $changelog" >&2; exit 1; }
fi
if [[ -n $set_version ]]; then
    if [[ -n $last ]] && ! dpkg --compare-versions "$set_version" gt "$last"; then
        echo "--set-version $set_version is not newer than $last" >&2
        exit 2
    fi
    version=$set_version
elif [[ $devbuild == yes ]]; then
    # Sorts after the release it's based on and before the next one.
    version=${last:-0.0.0}+dev$(date -u +%Y%m%d%H%M%S)
elif [[ -z $last ]]; then
    version=0.0.1
else
    IFS=. read -r major minor patch <<<"$last"
    version=$major.$minor.$((patch + 1))
fi

# Read the layout. Every non-"deb" destination is also installed by install_target.sh.
sources=() destinations=() modes=() conffiles=() target_paths=()
while read -r mode source destination flags; do
    [[ -z $mode || $mode == \#* ]] && continue
    [[ $mode == 0755 || $mode == 0644 ]] || { echo "Bad mode in files.list: $mode" >&2; exit 1; }
    [[ $destination == /* ]] || { echo "Bad destination in files.list: $destination" >&2; exit 1; }
    [[ $source == @BINARY@ ]] && source=$binary
    [[ -f $source && -r $source ]] || { echo "Missing file: $source" >&2; exit 1; }
    sources+=("$source") destinations+=("$destination") modes+=("$mode")
    [[ " ${flags:-} " == *" conf "* ]] && conffiles+=("$destination")
    [[ " ${flags:-} " == *" deb "* ]] || target_paths+=("$destination")
done < "$packaging/files.list"
previous_paths=()
while read -r path; do
    [[ -z $path || $path == \#* ]] && continue
    previous_paths+=("$path")
done < "$packaging/previous-layout"

# Checks before building.
file -b "$binary" | grep -q 'ELF 64-bit.*ARM aarch64' ||
    { echo "Not an aarch64 binary: $binary ($(file -b "$binary"))" >&2; exit 1; }
# The unit sets ARGUS_DATA_DIR, but a stale CMake cache keeps the old built-in default.
if grep -qaF /usr/share/argus "$binary"; then
    echo "Warning: $binary has /usr/share/argus built in; reconfigure with -DARGUS_DATA_DIR=/etc/argus" >&2
fi
for i in "${!sources[@]}"; do
    source=${sources[i]}
    case "$source" in
        *.json) python3 -m json.tool "$source" >/dev/null || { echo "Invalid JSON: $source" >&2; exit 1; } ;;
        *.sh) bash -n "$source" ;;
    esac
    if [[ $source != "$binary" && " ${target_paths[*]} " == *" ${destinations[i]} "* ]] &&
        grep -q '/usr/local' "$source"; then
        echo "$source refers to /usr/local; files.list paths must be used" >&2
        exit 1
    fi
done
for script in preinst postinst prerm postrm; do
    sh -n "$packaging/$script"
done
unit=scripts/sensor-payload.service
[[ $(sed -n 's/^WorkingDirectory=//p' "$unit") == /* ]] ||
    { echo "Expected an absolute WorkingDirectory in $unit" >&2; exit 1; }
grep -qx 'Environment=ARGUS_DATA_DIR=/etc/argus' "$unit" ||
    { echo "Expected Environment=ARGUS_DATA_DIR=/etc/argus in $unit" >&2; exit 1; }
uninstaller=$(sed 's/^[[:space:]]*//; s/[[:space:]]*$//' scripts/uninstall_target.sh)
for path in "${target_paths[@]}" "${previous_paths[@]}"; do
    grep -qxF -- "$path" <<<"$uninstaller" ||
        { echo "scripts/uninstall_target.sh does not list $path" >&2; exit 1; }
done

# Changelog entry for this build.
branch=$(git rev-parse --abbrev-ref HEAD)
[[ $branch != HEAD ]] || branch='HEAD (detached)'
commit=$(git rev-parse HEAD)
# Notable changes since the commit of the last release, from the commit subjects.
# Commits that only touch debian/changelog or documentation are left out.
max_changes=5
changes=()
if [[ -f $changelog ]]; then
    last_commit=$(awk '/^ -- /{exit} /^  \* Commit: /{print $3; exit}' "$changelog")
    last_commit=${last_commit%-dirty}
    if [[ -n $last_commit ]] && git rev-parse -q --verify "$last_commit^{commit}" >/dev/null &&
        git merge-base --is-ancestor "$last_commit" HEAD; then
        mapfile -t changes < <(git log --no-merges --format=%s "$last_commit..HEAD" -- . \
            ':(exclude)debian/changelog' ':(exclude)*.md')
    else
        echo "Warning: commit '$last_commit' of $last is not an ancestor of HEAD; no change summary" >&2
    fi
fi
# Only changes to tracked files count; untracked files are ignored.
if [[ -n $(git status --porcelain --untracked-files=no -- . ':(exclude)debian/changelog') ]]; then
    commit+=-dirty
fi
name=$(git config user.name) || { echo 'Set git config user.name' >&2; exit 1; }
email=$(git config user.email) || { echo 'Set git config user.email' >&2; exit 1; }
maintainer="$name <$email>"

stage=$(mktemp -d)
new_changelog=$(mktemp)
trap 'rm -rf -- "$stage" "$new_changelog"' EXIT
# The package ships this changelog; only releases also write it to debian/.
{
    printf 'sensor-payload (%s) unstable; urgency=low\n\n' "$version"
    if [[ $devbuild == yes ]]; then
        printf '  * Development build based on %s, not a release\n' "${last:-no release}"
    fi
    printf '  * Branch: %s\n  * Commit: %s\n' "$branch" "$commit"
    if ((${#changes[@]})); then
        printf '  * Changes since %s:\n' "$last"
        for change in "${changes[@]:0:max_changes}"; do
            ((${#change} <= 72)) || change="${change:0:69}..."
            printf '    - %s\n' "$change"
        done
        if ((${#changes[@]} > max_changes)); then
            printf '    - and %d more commits\n' $((${#changes[@]} - max_changes))
        fi
    fi
    echo
    printf ' -- %s  %s\n' "$maintainer" "$(date -R)"
    if [[ -f $changelog ]]; then
        echo
        cat "$changelog"
    fi
} > "$new_changelog"

# Stage the package tree.
chmod 755 "$stage"
for i in "${!sources[@]}"; do
    install -D -m "${modes[i]}" "${sources[i]}" "$stage${destinations[i]}"
done
doc="$stage/usr/share/doc/sensor-payload"
install -d -m 755 "$doc"
gzip -9n < "$new_changelog" > "$doc/changelog.Debian.gz"
chmod 644 "$doc/changelog.Debian.gz"

install -d -m 755 "$stage/DEBIAN"
paths=$(printf '%s\n' "${target_paths[@]}" "${previous_paths[@]}")
# Embed the layout in preinst; it runs before the package's files are unpacked.
awk -v paths="$paths" '$0 == "@ARGUS_PATHS@" { print paths; next } { print }' \
    "$packaging/preinst" > "$stage/DEBIAN/preinst"
chmod 755 "$stage/DEBIAN/preinst"
install -m 755 "$packaging/postinst" "$packaging/prerm" "$packaging/postrm" "$stage/DEBIAN/"
if ((${#conffiles[@]})); then
    printf '%s\n' "${conffiles[@]}" > "$stage/DEBIAN/conffiles"
    chmod 644 "$stage/DEBIAN/conffiles"
fi
installed_size=$(du -sk --exclude=DEBIAN "$stage" | cut -f1)
sed -e "s|@VERSION@|$version|" -e "s|@MAINTAINER@|$maintainer|" \
    -e "s|@INSTALLED_SIZE@|$installed_size|" "$packaging/control.in" > "$stage/DEBIAN/control"
(cd "$stage" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' | sort -z |
    xargs -0 md5sum) > "$stage/DEBIAN/md5sums"
chmod 644 "$stage/DEBIAN/control" "$stage/DEBIAN/md5sums"

mkdir -p dist
package="dist/sensor-payload_${version}_arm64.deb"
dpkg-deb --root-owner-group -Zxz --build "$stage" "$package"

echo "Built $package (v$version, $branch, $commit)"
if [[ $devbuild == yes ]]; then
    # Keep only the newest development build.
    for old in dist/sensor-payload_*+dev*_arm64.deb; do
        if [[ -f $old && $old != "$package" ]]; then
            rm -f -- "$old"
        fi
    done
else
    # Record the release only once the package exists.
    install -d -m 755 debian
    install -m 644 "$new_changelog" "$changelog"
    echo "Commit $changelog to record v$version"
fi
