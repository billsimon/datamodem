#!/usr/bin/env bash
# End-to-end smoke test: one datamodem answers, another dials it, and a line
# of text crosses real SIP signalling and real RTP on the loopback interface.
#
#   scripts/loopback-test.sh [path/to/datamodem]
#
# This exercises everything the in-memory `selftest` does not: SDP, codec
# negotiation, the jitter buffer, the pjmedia conference bridge, and the
# 20 ms frame cadence that the modems actually have to live with.
set -euo pipefail

BIN=${1:-build/datamodem}
WORK=$(mktemp -d -t datamodem-loopback)
ANS_PORT=${ANS_PORT:-5080}
CALL_PORT=${CALL_PORT:-5070}
ANS_RTP=${ANS_RTP:-4100}
CALL_RTP=${CALL_RTP:-4200}
MODULATION=${MODULATION:-v21}
BITRATE=${BITRATE:-}       # V.32 and V.22bis; empty means the most the modulation can do
MESSAGE=${MESSAGE:-"hello from the calling modem"}
# Extra flags for each end, so the same script can drive the asymmetric
# cases: V.42 offered by one side only, and so on.
CALL_FLAGS=${CALL_FLAGS:-}
ANS_FLAGS=${ANS_FLAGS:-}
# What the caller's log should end up saying. Empty means do not care.
EXPECT_CALL=${EXPECT_CALL:-}

cleanup() {
    [ -n "${ANS_PID:-}" ] && kill "$ANS_PID" 2>/dev/null || true
    wait "${ANS_PID:-}" 2>/dev/null || true
    echo "logs and output in $WORK"
}
trap cleanup EXIT

[ -x "$BIN" ] || {
    echo "no datamodem binary at $BIN - run: cmake -S . -B build && cmake --build build" >&2
    exit 1
}

RATE_FLAGS=()
[ -n "$BITRATE" ] && RATE_FLAGS=(--bit-rate "$BITRATE")

echo "==> in-memory selftest first ($MODULATION${BITRATE:+ at $BITRATE})"
"$BIN" selftest --modulation "$MODULATION" ${RATE_FLAGS[@]+"${RATE_FLAGS[@]}"}

echo "==> starting the answering modem on port $ANS_PORT"
# The answering side echoes whatever it is sent back at the caller, which is
# what makes the round trip checkable. Its stdout is the received data.
"$BIN" answer \
    --server 127.0.0.1 \
    --username ans \
    --no-register \
    --local-port "$ANS_PORT" \
    --rtp-port "$ANS_RTP" \
    --modulation "$MODULATION" \
    ${RATE_FLAGS[@]+"${RATE_FLAGS[@]}"} \
    --idle-timeout 20 \
    --max-call 120 \
    --log-file "$WORK/answer.log" \
    $ANS_FLAGS \
    </dev/null >"$WORK/answer.out" &
ANS_PID=$!

for _ in $(seq 1 100); do
    grep -q "waiting for an inbound call" "$WORK/answer.log" 2>/dev/null && break
    sleep 0.2
done

echo "==> dialling"
set +e
printf '%s\r\n' "$MESSAGE" | "$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
    --server 127.0.0.1 \
    --username call \
    --no-register \
    --local-port "$CALL_PORT" \
    --rtp-port "$CALL_RTP" \
    --modulation "$MODULATION" \
    ${RATE_FLAGS[@]+"${RATE_FLAGS[@]}"} \
    --idle-timeout 15 \
    --max-call 120 \
    --log-file "$WORK/dial.log" \
    $CALL_FLAGS \
    >"$WORK/dial.out"
DIAL_RC=$?
set -e

wait "$ANS_PID" 2>/dev/null || true
ANS_PID=""

echo "==> what the answering modem received:"
cat -v "$WORK/answer.out"

fail=0
LC_ALL=C grep -a -qF "$MESSAGE" "$WORK/answer.out" || {
    echo "==> FAIL: the message did not arrive" >&2; fail=1; }

if [ -n "$EXPECT_CALL" ]; then
    if grep -qE "$EXPECT_CALL" "$WORK/dial.log"; then
        echo "==> the caller's log says: $(grep -oE "$EXPECT_CALL" "$WORK/dial.log" | head -1)"
    else
        echo "==> FAIL: expected /$EXPECT_CALL/ in the caller's log" >&2; fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    echo "==> PASS: the message crossed the call intact (dial exit $DIAL_RC)"
    exit 0
fi

echo "==> FAILED (dial exit $DIAL_RC)" >&2
echo "--- dial.log (tail) ---" >&2
tail -30 "$WORK/dial.log" >&2 || true
echo "--- answer.log (tail) ---" >&2
tail -30 "$WORK/answer.log" >&2 || true
exit 1
