#!/usr/bin/env bash
# Real far ends, replayed: each recording in a directory (default
# recordings/, kept out of git - they are a megabyte or two a call) played
# to a calling modem with DATAMODEM_REPLAY, which has to link up again. A
# recording named <modulation>-<anything>.wav has to link up in that
# modulation.
#
#   scripts/replay-test.sh [path/to/datamodem] [directory]
#
# Make one with DATAMODEM_RECORD=/tmp/call on a real call; the far end is
# the .out.wav.
set -uo pipefail

BIN=${1:-build/datamodem}
DIR=${2:-recordings}
[ -x "$BIN" ] || { echo "no datamodem binary at $BIN" >&2; exit 1; }

pass=0
fail=0
shopt -s nullglob
for wav in "$DIR"/*.wav; do
    name=$(basename "$wav" .wav)
    want=${name%%-*}
    out=$(DATAMODEM_REPLAY=$wav "$BIN" selftest 2>&1)
    result=$(printf '%s\n' "$out" | grep -oE "replay modulation=[^ ]+ rate=[0-9]+ protocol=[^ ]+ link=[a-z]+" | tail -1)
    got=$(printf '%s\n' "$result" | grep -oE "modulation=[^ ]+" | cut -d= -f2)
    if printf '%s\n' "$result" | grep -q "link=up" && { [ "$got" = "$want" ] || ! printf '%s' "$want" | grep -qE '^(v34|v32bis|v32|v22bis|v22|v21|bell103|v23)$'; }; then
        pass=$((pass + 1))
        printf 'PASS  %-40s %s\n' "$name" "${result#replay }"
    else
        fail=$((fail + 1))
        printf 'FAIL  %-40s %s\n' "$name" "${result:-no result}"
        printf '%s\n' "$out" | grep -E "selftest\]|error" | tail -8 | sed 's/^/        /'
    fi
done
[ $((pass + fail)) -gt 0 ] || { echo "no recordings in $DIR"; exit 0; }
echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
