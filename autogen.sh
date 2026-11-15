#!/bin/sh
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build-aux m4
autoreconf --install --force --warnings=all "$@"
echo "now run ./configure && make"
