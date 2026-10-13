#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

NSP=${NSP:-metroid_dread_rom.nsp}
KEYS=${KEYS:-$HOME/.switch/prod.keys}
HACTOOL=./tools/hactool
WORK=build/nsp
OUT=extracted

[[ -f $KEYS ]] || { echo "prod.keys não encontrado em $KEYS (dump do seu console via Lockpick_RCM)"; exit 1; }
CLEAN_KEYS=$(mktemp); trap 'rm -f "$CLEAN_KEYS"' EXIT
awk -F' = ' '!/_kek_source_/ || length($2)==32' "$KEYS" > "$CLEAN_KEYS"
hac() { "$HACTOOL" -k "$CLEAN_KEYS" "$@" 2> >(grep -v '^\[WARN\]' >&2); }

mkdir -p "$WORK" "$OUT"
hac -t pfs0 --outdir="$WORK" "$NSP"

TIK=$(ls "$WORK"/*.tik)
TITLEKEY=$(od -An -tx1 -j $((0x180)) -N16 "$TIK" | tr -d ' \n')

for nca in "$WORK"/*.nca; do
    [[ $nca == *.cnmt.nca ]] && continue
    type=$(hac --titlekey="$TITLEKEY" "$nca" | sed -n 's/^Content Type: *//p' | head -1)
    case $type in
        Program) hac --titlekey="$TITLEKEY" --exefsdir="$OUT/exefs" --romfsdir="$OUT/romfs" "$nca" ;;
        Control) hac --titlekey="$TITLEKEY" --romfsdir="$OUT/control" "$nca" ;;
    esac
done

mkdir -p "$OUT/nso"
for nso in main sdk subsdk0; do
    [[ -f $OUT/exefs/$nso ]] && hac -t nso --uncompressed="$OUT/nso/$nso.raw" "$OUT/exefs/$nso"
done

rm -rf "$WORK"
echo "ok: $OUT/exefs  $OUT/romfs  $OUT/nso/*.raw"
