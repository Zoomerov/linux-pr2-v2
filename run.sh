#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$root" -B "$root/.build" >/dev/null
cmake --build "$root/.build" >/dev/null
exec "$root/.build/nuget-graph" "$@"
