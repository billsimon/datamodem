#!/usr/bin/env bash
# End-to-end test of answering for a program: one datamodem answers with
# --exec and --calls 3, another dials it three times over loopback SIP.
#
#   scripts/exec-test.sh [path/to/datamodem]
#
# Calls 1 and 2 check that the command
#   - starts once the link is up, with the line as its stdin and stdout,
#   - sees the caller ID, the number called, and the connect result,
#   - hears what the caller sends, and is heard by it,
# that the call is cleared as soon as the command exits rather than after
# the 30 s an unattended run waits for a reply, and that the one answering
# modem takes the next call without restarting. Call 3 hangs up on the
# command, which must then be stopped.
set -euo pipefail

BIN=${1:-./datamodem}
WORK=$(mktemp -d -t datamodem-exec)
ANS_PORT=${ANS_PORT:-5180}
CALL_PORT=${CALL_PORT:-5170}
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

# The "BBS": greets the caller with what it knows of the call, echoes one
# line back, and says goodbye. It also proves the SIP password is not
# passed on to it.
cat >"$WORK/bbs.sh" <<'EOF'
#!/bin/sh
printf 'welcome caller=[%s] name=[%s] called=[%s] connect=[%s] rate=[%s] password=[%s]\r\n' \
    "$CALLER_ID" "$CALLER_NAME" "$CALLED_ID" "$CONNECT" "$MODEM_RATE" "${DATAMODEM_PASSWORD:-}"
IFS= read -r line
printf 'you said [%s]\r\ngoodbye\r\n' "$(printf '%s' "$line" | tr -d '\r')"
EOF
chmod +x "$WORK/bbs.sh"

echo "==> starting the answering modem on port $ANS_PORT (--calls 3)"
DATAMODEM_PASSWORD=sekrit "$BIN" answer \
    --server 127.0.0.1 --username ans --no-register \
    --local-port "$ANS_PORT" --rtp-port "$ANS_RTP" \
    --modulation "$MODULATION" \
    --calls 3 \
    --exec "$WORK/bbs.sh" \
    --max-call 120 \
    --log-file "$WORK/answer.log" \
    </dev/null >"$WORK/answer.out" 2>&1 &
ANS_PID=$!

for _ in $(seq 1 100); do
    grep -q "waiting for an inbound call" "$WORK/answer.log" 2>/dev/null && break
    sleep 0.2
done

fail=0
for call in 1 2; do
    echo "==> call $call"
    start=$(date +%s)
    set +e
    # Holds its input open for a while, as a person would: the call must end
    # because the answering command exits, not because our input did.
    "$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
        --server 127.0.0.1 --username call --no-register \
        --caller-id "555012$call" \
        --local-port "$CALL_PORT" --rtp-port "$CALL_RTP" \
        --modulation "$MODULATION" \
        --max-call 120 \
        --log-file "$WORK/dial$call.log" \
        < <(printf 'hello from call %s\r\n' "$call"; sleep 40) \
        >"$WORK/dial$call.out"
    rc=$?
    set -e
    took=$(( $(date +%s) - start ))
    cat -v "$WORK/dial$call.out"
    for want in "caller=\[555012$call\]" "called=\[ans\]" "connect=\[300 V\.42/V\.42bis\]" "rate=\[300\]" \
                "password=\[\]" "you said \[hello from call $call\]" "goodbye"; do
        LC_ALL=C grep -a -qE "$want" "$WORK/dial$call.out" || {
            echo "==> FAIL: call $call: expected /$want/" >&2; fail=1; }
    done
    if [ "$took" -ge 35 ]; then
        echo "==> FAIL: call $call took ${took}s; it should end when the command exits" >&2
        fail=1
    fi
    echo "==> call $call: dial exit $rc after ${took}s"
done

# Call 3: the caller has nothing to say, and hangs up when its input ends
# eight seconds in (--hangup-on-eof), while the command is still waiting for
# a line. The
# answering end must stop the command - its stdin ends and it gets SIGHUP -
# and the caller must not wait out the 30 s an unattended run would.
echo "==> call 3 (caller hangs up first)"
start=$(date +%s)
set +e
"$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
    --server 127.0.0.1 --username call --no-register \
    --local-port "$CALL_PORT" --rtp-port "$CALL_RTP" \
    --modulation "$MODULATION" \
    --hangup-on-eof \
    --max-call 120 \
    --log-file "$WORK/dial3.log" \
    < <(sleep 8) >"$WORK/dial3.out"
rc=$?
set -e
took=$(( $(date +%s) - start ))
echo "==> call 3: dial exit $rc after ${took}s"
if [ "$took" -ge 25 ]; then
    echo "==> FAIL: call 3 took ${took}s; --hangup-on-eof should clear it at once" >&2
    fail=1
fi

# Three calls taken: the answering modem should now have exited by itself.
for _ in $(seq 1 50); do
    kill -0 "$ANS_PID" 2>/dev/null || break
    sleep 0.2
done
if kill -0 "$ANS_PID" 2>/dev/null; then
    echo "==> FAIL: the answering modem is still running after --calls 3" >&2
    fail=1
else
    ans_rc=0
    wait "$ANS_PID" || ans_rc=$?
    ANS_PID=""
    echo "==> answering modem exited $ans_rc"
fi
grep -c 'exec.*finished' "$WORK/answer.log" | grep -qx 3 || {
    echo "==> FAIL: expected the command to have run and finished three times" >&2; fail=1; }
# Hung up on mid-read, the third must have been stopped by the hangup
# rather than finishing by itself.
grep 'exec.*finished' "$WORK/answer.log" | tail -1 | grep -q 'signal 1' || {
    echo "==> FAIL: expected the third command to be ended by SIGHUP" >&2; fail=1; }

if [ "$fail" -eq 0 ]; then
    echo "==> PASS"
    exit 0
fi
echo "--- answer.log (tail) ---" >&2
tail -40 "$WORK/answer.log" >&2 || true
exit 1
