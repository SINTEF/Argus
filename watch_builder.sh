#!/usr/bin/env bash
# Compatibility entry point; defaults to Raspberry Pi build and installation.
set -euo pipefail
exec "$(dirname -- "${BASH_SOURCE[0]}")/scripts/watch_builder.sh" "$@"
