#!/bin/sh
# Fails if libbifrost exports anything other than the bf_* C API.
set -eu
lib="$1"
extra=$(nm -gU "$lib" | awk '{print $NF}' | grep -v '^_bf_' || true)
if [ -n "$extra" ]; then
    echo "unexpected exported symbols:" >&2
    echo "$extra" | head -20 >&2
    exit 1
fi
count=$(nm -gU "$lib" | grep -c ' _bf_' || true)
[ "$count" -gt 40 ] || { echo "only $count bf_ symbols exported" >&2; exit 1; }
