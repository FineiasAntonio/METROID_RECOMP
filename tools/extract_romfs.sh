#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
KEYS=$(mktemp); WORK=build/nsp
trap 'rm -rf "$KEYS" "$WORK"' EXIT
awk -F' = ' '!/_kek_source_/ || length($2)==32' "${KEYS_FILE:-$HOME/.switch/prod.keys}" > "$KEYS"
hac() { ./tools/hactool -k "$KEYS" "$@" 2> >(grep -v '^\[WARN\]' >&2); }
mkdir -p "$WORK"
hac -t pfs0 --outdir="$WORK" metroid_dread_rom.nsp
TITLEKEY=$(od -An -tx1 -j $((0x180)) -N16 "$WORK"/*.tik | tr -d ' \n')
for nca in "$WORK"/*.nca; do
    [[ $nca == *.cnmt.nca ]] && continue
    if hac --titlekey="$TITLEKEY" "$nca" | grep -q "Content Type: *Program"; then
        hac --titlekey="$TITLEKEY" --romfs=extracted/romfs.bin "$nca"
    fi
done
ls -la extracted/romfs.bin
