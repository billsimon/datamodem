#!/usr/bin/env bash
# Exercises the Hayes escape over a real call: send some data, pause, type
# +++, pause, then drive the AT prompt and hang up. Checks the two things
# that actually matter - that the escape is noticed, and that the three
# plus signs do NOT reach the far end.
#
#   scripts/escape-test.sh [path/to/datamodem]
set -euo pipefail

BIN=${1:-build/datamodem}
WORK=$(mktemp -d -t datamodem-escape)
ANS_PORT=${ANS_PORT:-5081}
CALL_PORT=${CALL_PORT:-5071}
ANS_RTP=${ANS_RTP:-4300}
CALL_RTP=${CALL_RTP:-4400}
MODULATION=${MODULATION:-v21}

cleanup() {
    [ -n "${ANS_PID:-}" ] && kill "$ANS_PID" 2>/dev/null || true
    wait "${ANS_PID:-}" 2>/dev/null || true
    echo "logs and output in $WORK"
}
trap cleanup EXIT

[ -x "$BIN" ] || { echo "no datamodem binary at $BIN" >&2; exit 1; }

"$BIN" answer \
    --server 127.0.0.1 --username ans --no-register \
    --local-port "$ANS_PORT" --rtp-port "$ANS_RTP" \
    --modulation "$MODULATION" --max-call 90 \
    --log-file "$WORK/answer.log" </dev/null >"$WORK/answer.out" &
ANS_PID=$!

for _ in $(seq 1 100); do
    grep -q "waiting for an inbound call" "$WORK/answer.log" 2>/dev/null && break
    sleep 0.2
done

# The pauses either side of +++ are the point of the exercise, so the script
# has to stay quiet across them - hence one long sleep first, to let the call
# train before any of this timing starts to matter.
{
    sleep 12
    printf 'before-escape\r'
    sleep 2
    printf '+++'          # no CR: the escape sequence is three bare characters
    sleep 2
    printf 'ATI\r'
    sleep 1
    printf 'ATH\r'
    sleep 2
} | "$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
    --server 127.0.0.1 --username call --no-register \
    --local-port "$CALL_PORT" --rtp-port "$CALL_RTP" \
    --modulation "$MODULATION" --max-call 90 \
    --log-file "$WORK/dial.log" >"$WORK/dial.out" 2>"$WORK/dial.term" || true

wait "$ANS_PID" 2>/dev/null || true
ANS_PID=""

echo "==> local terminal output from the caller:"
sed 's/^/    /' "$WORK/dial.term"
echo "==> what the answering modem received:"
cat -v "$WORK/answer.out" | sed 's/^/    /'

fail=0
LC_ALL=C grep -a -qF 'before-escape' "$WORK/answer.out" || { echo "FAIL: the data never arrived" >&2; fail=1; }
LC_ALL=C grep -a -qF '+++' "$WORK/answer.out" && { echo "FAIL: +++ was forwarded to the far end" >&2; fail=1; }
grep -q 'OK' "$WORK/dial.term" || { echo "FAIL: never reached command mode" >&2; fail=1; }
grep -q 'modulation ' "$WORK/dial.term" || { echo "FAIL: ATI produced nothing" >&2; fail=1; }
grep -q 'hung up locally' "$WORK/dial.term" || { echo "FAIL: ATH did not hang up" >&2; fail=1; }

[ "$fail" -eq 0 ] && { echo "==> PASS"; exit 0; }
echo "--- dial.log (tail) ---" >&2; tail -20 "$WORK/dial.log" >&2
exit 1
