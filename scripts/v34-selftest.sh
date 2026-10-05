#!/usr/bin/env bash
# V.34 against itself, over the selftest's simulated line, in every
# configuration that a clean line would never exercise: each symbol rate,
# both carriers, the pre-emphasis filters, echo and long round trips through
# G.711, noise, bursts, and V.42/V.42bis on top.
#
#   scripts/v34-selftest.sh [path/to/datamodem]
#
# Each case prints what it trained at and whether the data came through
# intact. Takes about a minute.
set -uo pipefail

BIN=${1:-build/datamodem}
[ -x "$BIN" ] || { echo "no datamodem binary at $BIN" >&2; exit 1; }

pass=0
fail=0

# case NAME [VAR=value ...] -- [datamodem selftest arguments]
case_() {
    local name=$1; shift
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    [ "${1:-}" = "--" ] && shift
    local out rc
    out=$(env ${envs[@]+"${envs[@]}"} "$BIN" selftest --modulation v34 "$@" 2>&1)
    rc=$?
    local trained result
    trained=$(printf '%s\n' "$out" | grep -oE "trained: [0-9]+ bit/s out at [0-9]+ symbols/s on [0-9]+ Hz, [0-9]+ bit/s in" | head -1)
    result=$(printf '%s\n' "$out" | grep -oE "protocol=[^ ]+ link_seconds=[0-9.-]+" | head -1)
    if [ $rc -eq 0 ]; then
        pass=$((pass + 1))
        printf 'PASS  %-34s %s  %s\n' "$name" "${trained#trained: }" "$result"
    else
        fail=$((fail + 1))
        printf 'FAIL  %-34s exit %d  %s\n' "$name" "$rc" "${trained#trained: }"
        printf '%s\n' "$out" | grep -E "error|warn" | tail -5 | sed 's/^/        /'
    fi
}

case_ "perfect line"
case_ "G.711"                         DATAMODEM_SELFTEST_LINE=ulaw
case_ "G.711, 300 ms round trip"      DATAMODEM_SELFTEST_LINE=ulaw,delay=150
case_ "G.711, 700 ms round trip"      DATAMODEM_SELFTEST_LINE=ulaw,delay=350
case_ "echo -10 dB, 300 ms"           DATAMODEM_SELFTEST_LINE=ulaw,delay=150,echo=-10
case_ "echo -6 dB, 100 ms"            DATAMODEM_SELFTEST_LINE=ulaw,delay=50,echo=-6
case_ "noise -40 dBm0"                DATAMODEM_SELFTEST_LINE=noise=-40
case_ "noise -30 dBm0"                DATAMODEM_SELFTEST_LINE=noise=-30
for sr in 0x01 0x02 0x04 0x08 0x10 0x20; do
    case_ "symbol rates $sr, low carrier"  DATAMODEM_V34_SYMBOL_RATES=$sr DATAMODEM_V34_CARRIER=low DATAMODEM_SELFTEST_LINE=ulaw
    case_ "symbol rates $sr, high carrier" DATAMODEM_V34_SYMBOL_RATES=$sr DATAMODEM_V34_CARRIER=high DATAMODEM_SELFTEST_LINE=ulaw
done
for pe in 2 5 7 10; do
    case_ "pre-emphasis $pe"              DATAMODEM_V34_PRE_EMPHASIS=$pe DATAMODEM_SELFTEST_LINE=ulaw
done
case_ "32-state trellis"              DATAMODEM_V34_TRELLIS=32 DATAMODEM_SELFTEST_BYTES=20000 DATAMODEM_SELFTEST_LINE=ulaw
case_ "64-state trellis"              DATAMODEM_V34_TRELLIS=64 DATAMODEM_SELFTEST_BYTES=20000 DATAMODEM_SELFTEST_LINE=ulaw
case_ "expanded shaping"              DATAMODEM_V34_SHAPING=1 DATAMODEM_SELFTEST_BYTES=20000 DATAMODEM_SELFTEST_LINE=ulaw
case_ "64 states, shaping, 2743 baud" DATAMODEM_V34_TRELLIS=64 DATAMODEM_V34_SHAPING=1 DATAMODEM_V34_SYMBOL_RATES=0x02 DATAMODEM_SELFTEST_LINE=ulaw
case_ "V.42"                          DATAMODEM_SELFTEST_LINE=ulaw,delay=100 -- --v42 require
case_ "V.42bis"                       DATAMODEM_SELFTEST_LINE=ulaw,delay=100 -- --v42 require --v42bis
case_ "V.42, bursts of noise, 60 kB"  DATAMODEM_SELFTEST_BYTES=60000 DATAMODEM_SELFTEST_LINE=ulaw,burst=20/3 -- --v42 require
case_ "V.42, echo, 700 ms, 20 kB"     DATAMODEM_SELFTEST_BYTES=20000 DATAMODEM_SELFTEST_LINE=ulaw,delay=350,echo=-12 -- --v42 require
case_ "V.42, 150 ms dropout, 80 kB"   DATAMODEM_SELFTEST_BYTES=80000 DATAMODEM_SELFTEST_LINE=ulaw,delay=100,cut=15/150 -- --v42 require
case_ "V.42, 2 s dropout: retrain"    DATAMODEM_SELFTEST_BYTES=80000 DATAMODEM_SELFTEST_LINE=ulaw,delay=100,cut=15/2000 -- --v42 require
case_ "renegotiate down, caller asks" DATAMODEM_V34_RENEGOTIATE=2:14400 DATAMODEM_SELFTEST_BYTES=40000 DATAMODEM_SELFTEST_LINE=ulaw,delay=100 -- --v42 require
case_ "renegotiate, answerer asks"    DATAMODEM_V34_RENEGOTIATE=2:19200:answer DATAMODEM_SELFTEST_BYTES=40000 DATAMODEM_SELFTEST_LINE=ulaw,delay=100 -- --v42 require
case_ "clock +100 ppm, V.42, 100 kB"  DATAMODEM_SELFTEST_BYTES=100000 DATAMODEM_SELFTEST_LINE=ulaw,drift=100 -- --v42 require
case_ "clock -100 ppm, V.42, 100 kB"  DATAMODEM_SELFTEST_BYTES=100000 DATAMODEM_SELFTEST_LINE=ulaw,drift=-100 -- --v42 require
case_ "clock 40 ppm, echo, 300 ms"    DATAMODEM_SELFTEST_BYTES=50000 DATAMODEM_SELFTEST_LINE=ulaw,drift=40,delay=150,echo=-12 -- --v42 require
case_ "async, 200 kB"                 DATAMODEM_SELFTEST_BYTES=200000 DATAMODEM_SELFTEST_LINE=ulaw
case_ "rate ceiling 14400"            DATAMODEM_SELFTEST_LINE=ulaw -- --bit-rate 14400
case_ "rate ceiling 2400"             DATAMODEM_SELFTEST_LINE=ulaw -- --bit-rate 2400

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
