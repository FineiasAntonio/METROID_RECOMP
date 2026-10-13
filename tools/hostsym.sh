#!/usr/bin/env bash
off=$(( $1 ))
nm -n build/dread | awk -v o="$off" '$2 ~ /[tT]/ { a = strtonum("0x" $1); if (a <= o) { best = $3; ba = a } else { printf "%s+%#x\n", best, o - ba; exit } }'
