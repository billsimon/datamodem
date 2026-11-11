#!/usr/bin/env bash
# What V.32 bis and V.34 do when a line goes bad in the middle of a call,
# over the selftest's simulated line: step down rather than retrain where a
# rate change will do, remember a rate that failed rather than go straight
# back to it, and not hold against a rate a failure that no rate would have
# survived. Each case checks what the log says happened as well as that the
# data came through intact.
#
#   scripts/retrain-test.sh [path/to/datamodem]
#
# Takes about half a minute.
set -uo pipefail

BIN=${1:-build/datamodem}
[ -x "$BIN" ] || { echo "no datamodem binary at $BIN" >&2; exit 1; }

pass=0
fail=0

# case NAME MODULATION [VAR=value ...] -- CHECK...
#
# Each CHECK is "+pattern" (must appear in the log), "-pattern" (must not),
# or "<n:pattern" (fewer than n lines match). Patterns are extended regular
# expressions. The transfer is always uncompressed, so that it lasts long
# enough to meet the line's bad patches.
case_() {
    local name=$1 mod=$2; shift 2
    local envs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    [ "${1:-}" = "--" ] && shift
    local out rc why=""
    out=$(env ${envs[@]+"${envs[@]}"} "$BIN" selftest --modulation "$mod" --no-v42bis 2>&1)
    rc=$?
    [ $rc -eq 0 ] || why="exit $rc"
    for check in "$@"; do
        [ -n "$why" ] && break
        case $check in
        +*) printf '%s\n' "$out" | grep -qE -- "${check#+}" || why="never said: ${check#+}" ;;
        -*) ! printf '%s\n' "$out" | grep -qE -- "${check#-}" || why="said: ${check#-}" ;;
        \<*)
            local spec=${check#<} n
            n=$(printf '%s\n' "$out" | grep -cE -- "${spec#*:}")
            [ "$n" -lt "${spec%%:*}" ] || why="$n times: ${spec#*:}"
            ;;
        esac
    done
    local result
    result=$(printf '%s\n' "$out" | grep -oE "rate=[0-9]+ protocol=[^ ]+ .*simulated_seconds=[0-9.]+" | head -1 |
             sed 's/ link_seconds=[0-9.]*//')
    if [ -z "$why" ]; then
        pass=$((pass + 1))
        printf 'PASS  %-46s %s\n' "$name" "$result"
    else
        fail=$((fail + 1))
        printf 'FAIL  %-46s %s\n' "$name" "$why"
        printf '%s\n' "$out" | grep -E "\[(v32|v34|session)\]|error" | grep -vE "probed|V\.8" | tail -12 |
            sed 's/^/        /'
    fi
}

# A line that is fine but for four seconds in every thirty, when the noise
# comes up far enough to break 14400 and not 9600: one step down, inside a
# second, and 14400 left alone afterwards - not stepped back up to ten
# seconds later, to fail at the next bad patch.
case_ "V.32 bis: bad patches, a step down will do" v32bis \
    DATAMODEM_SELFTEST_LINE=ulaw,delay=100,fade=4/30/-32 DATAMODEM_SELFTEST_BYTES=200000 -- \
    "+14400 did not hold - our reception gave out; staying below it for 5 minutes" \
    "+poor .* for 1\.0 seconds; asking the far end to change" \
    "-asking the far end to change to 14400" \
    "-; retraining \\("

# The same, so deep that nothing survives it: a retrain each time, as
# before, and no rate held against it - or the call would walk down to 4800.
case_ "V.32 bis: dropouts no rate survives" v32bis \
    DATAMODEM_SELFTEST_LINE=ulaw,delay=100,fade=3/40/-12 DATAMODEM_SELFTEST_BYTES=200000 -- \
    "+; retraining \\(" \
    "-did not hold" \
    "-trained at (12000|9600|7200|4800)"

# A step up the far end's reception will not bear - which is the far end's
# to judge, and a datamodem would not ask it; the test hook does. On
# probation, it steps straight back.
case_ "V.32 bis: a step up that does not hold" v32bis \
    DATAMODEM_V32_RENEGOTIATE=15:14400 DATAMODEM_SELFTEST_LINE=ulaw,delay=100,noise=-36/call \
    DATAMODEM_SELFTEST_BYTES=100000 -- \
    "+changed rate to 14400" \
    "+reception at 14400 is short of it" \
    "+14400 did not hold - the far end asked for less" \
    "-; retraining \\("

# V.34 over bad patches: renegotiation (11.6), not a retrain.
case_ "V.34: bad patches, a renegotiation will do" v34 \
    DATAMODEM_SELFTEST_LINE=ulaw,delay=100,fade=4/30/-45 DATAMODEM_SELFTEST_BYTES=400000 -- \
    "+33600 bit/s in did not hold - our reception gave out" \
    "+asking the far end to change rate \(to 31200" \
    "+rates changed without a retrain" \
    "-; retraining \\("

case_ "V.34: dropouts no rate survives" v34 \
    DATAMODEM_SELFTEST_LINE=ulaw,delay=100,fade=3/40/-12 DATAMODEM_SELFTEST_BYTES=400000 -- \
    "+; retraining \\(" \
    "-did not hold" \
    "-renegotiating"

# A far end that retrains whenever its reception is poor, at whatever rate
# training gives it, as the one did that a BBS hung up on after four of
# them in two minutes: it is our transmitter that has to slow down.
case_ "V.34: a far end that retrains at the same rate" v34 \
    DATAMODEM_V34_PLAIN_RECOVERY=call DATAMODEM_SELFTEST_LINE=ulaw,delay=100,fade=4/20/-45/call \
    DATAMODEM_SELFTEST_BYTES=400000 -- \
    "+33600 bit/s out did not hold - the far end retrained" \
    "+trained: 31200 bit/s out" \
    "<3:answering a retrain"

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
