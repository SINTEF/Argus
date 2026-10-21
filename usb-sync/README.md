# Argus USB log export

On insertion, a USB filesystem is mounted temporarily and the contents of
`/var/log/argus/` are copied into `argus/` at the root of the stick. Nested and
hidden files are included. Both partitioned sticks and whole-device filesystems
are supported. Every added USB block device triggers the service. `mount`
automatically detects the filesystem, including FAT32 and exFAT when supported
by the target system. Non-USB devices do not trigger the rule.

For partitioned sticks, both the disk (for example `/dev/sda`) and its partitions
(such as `/dev/sda1`) trigger separate services. The whole-disk mount may fail
because it contains a partition table; the partition service still runs. Mount
failures are recorded in the service journal.

The udev rule starts a systemd service because mounting and long-running copies
must run outside the udev worker. If the device is already mounted (for example
under `/media/pi/`), the script copies into `argus/` at that mount and leaves it
mounted. Otherwise it creates a temporary mount and unmounts it after copying.
Completion is reported in the journal. Wait for completion before removing the
stick, and safely unmount it yourself if its existing mount was reused.

## Installation

These files are installed by the `sensor-payload` Debian package
(`scripts/build-dpkg.sh`) and by `scripts/install_target.sh`, at the same paths
(`packaging/files.list`):

- `argus-usb-sync.sh` → `/usr/sbin/argus-usb-sync`
- `argus-usb-sync@.service` → `/lib/systemd/system/`
- `99-argus-usb-sync.rules` → `/etc/udev/rules.d/`
- `argus-update.sh` → `/usr/sbin/argus-update`
- `argus-update.service` → `/lib/systemd/system/`

Both reload systemd and udev. Installation does not start an export. Remove them
with `sudo dpkg --purge sensor-payload` or `scripts/uninstall_target.sh`; both
block new exports and stop running ones before stopping the payload. Source logs
and copies on USB storage are preserved.

The export needs rsync, util-linux (`findmnt`, `flock`) and `mount`; the package
depends on them.

## Run and inspect

Insert (or unplug and reinsert) the stick after installation. No `systemctl enable`
is needed; udev starts the service. Each USB block device gets its own service
instance, with copies serialized by a lock.

For a stick whose filesystem is `/dev/sda1`:

```sh
journalctl -u argus-usb-sync@sda1.service
sudo systemctl start argus-usb-sync@sda1.service
```

The second command manually repeats the export while the stick remains inserted.

Rsync verbose, itemized output is always enabled. Each transferred path and its
transferred byte count are recorded in the service journal. Existing files are
skipped and are not listed as transfers. To follow the log live (use `sda` instead
of `sda1` for a whole-device filesystem):

```sh
sudo journalctl -f -u argus-usb-sync@sda1.service
```

## Updating from a USB stick

The export can also upgrade the payload in the field. Enable it once per target:

```sh
sudo touch /etc/argus/allow-usb-upgrade    # remove the file to disable
```

Then put a newer package, as built by `scripts/build-dpkg.sh`, in the root
directory of the stick (for example `sensor-payload_0.0.6_arm64.deb`) and insert
it. After the logs are copied:

1. The script considers `sensor-payload_*_arm64.deb` files in the stick's root.
   Each must really be a `sensor-payload` package for `arm64`; the highest
   version wins.
2. It is used only if it is newer than the installed version; there is no
   reinstall or downgrade from USB. A development build
   (`build-dpkg.sh --devbuild`, version `<release>+dev<timestamp>`) counts as
   newer than its release.
3. The package is copied to `/var/cache/argus-update/` and the stick is
   unmounted. The journal then says the stick can be removed.
4. `argus-update.service` installs the package with
   `dpkg --force-confdef --force-confold -i`, which keeps configuration edited on
   the target. It waits for the export to finish, and new exports wait for the
   install. The payload is stopped during the install and restarted afterwards.

Without `/etc/argus/allow-usb-upgrade`, packages on the stick are ignored and the
export logs `USB upgrade disabled`. Packages are not signed: anyone with physical
access to the target can install software this way, as they could anyway. On a
target set up with `install_target.sh`, the package refuses to install (see
`INSTALL_TARGET.md`); clean it with `uninstall_target.sh` first.

Follow the install with:

```sh
journalctl -u argus-update.service
```

## Copy behavior

Colons in exported file and directory names become underscores. For example,
`argus-2026-09-23_09:58:58.log` is copied as
`argus-2026-09-23_09_58_58.log`. Source names and contents remain unchanged.
Existing destination files are checked using the translated names. If two source
paths translate to the same name (such as `a:b.log` and `a_b.log`), the export
fails before copying rather than silently skipping one. Name translations are
logged. Other filesystem naming restrictions still apply.

[`rsync --ignore-existing`](https://download.samba.org/pub/rsync/rsync.1)
copies only missing destination paths. Existing files are never updated, even
when their source size, contents or modification time changes. No reverse sync,
destination-only comparison pass, deletion, or source removal is performed.
New files inside existing directories are still copied.

This runs once per insertion, not continuously. After mounting the stick, the
script checks whether `sensor-payload.service` is active. If so, it stops the
service before copying and starts it again after the copy. If the export fails
or is interrupted, cleanup also attempts to restart the service. A service that
was inactive is left inactive. Stop failures abort the copy; restart failures
are reported in the journal and cause the export to fail.

Existing destination files remain unchanged on later exports, even if the source
has grown since the previous copy. Use rotated, uniquely named log files when
complete logs are required.

Symlinks are copied as their target contents, including targets outside the log
directory. Unix ownership, permissions, special files and symlinks themselves
cannot be represented on FAT32. FAT32 also limits individual files to less than
4 GiB and has case-insensitive names. Copy errors are reported as service failures;
completed files remain and a later run retries missing files.
