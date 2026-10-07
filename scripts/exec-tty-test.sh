#!/usr/bin/env bash
# End-to-end test of --exec-tty: one datamodem answers with --exec-tty and
# --calls 2, another dials it twice over loopback SIP.
#
#   scripts/exec-tty-test.sh [path/to/datamodem]
#
# Call 1 checks that the command
#   - has a terminal for its stdin, stdout and stderr, and as its
#     controlling terminal, 80x24 at the connect rate,
#   - gets the cooked line a serial port would give it: what the caller
#     types is echoed, its CR arrives as NL, and NL goes out as CR LF,
#   - can turn echo off, as login does for a password,
# and that the call is cleared once it exits. In call 2 the caller hangs up
# on a command that is waiting for a line, which must then be stopped.
set -euo pipefail

BIN=${1:-./datamodem}
WORK=$(mktemp -d -t datamodem-exec-tty)
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

# A stand-in for login: says what kind of line it has, asks for a name and
# then, without echo, a secret. Plain \n line ends throughout: the terminal
# is what turns them into CR LF.
cat >"$WORK/login.sh" <<'EOF'
#!/bin/sh
if [ -t 0 ] && [ -t 1 ] && [ -t 2 ] && (: </dev/tty) 2>/dev/null; then tty=yes; else tty=no; fi
printf 'tty=[%s] size=[%s] speed=[%s]\n' "$tty" "$(stty size)" "$(stty speed)"
printf 'name? '
IFS= read -r name
stty -echo
printf 'secret? '
IFS= read -r secret
stty echo
printf '\nhello [%s] secret has %d characters\n' "$name" "${#secret}"
EOF
chmod +x "$WORK/login.sh"

echo "==> starting the answering modem on port $ANS_PORT (--exec-tty --calls 2)"
"$BIN" answer \
    --server 127.0.0.1 --username ans --no-register \
    --local-port "$ANS_PORT" --rtp-port "$ANS_RTP" \
    --modulation "$MODULATION" \
    --calls 2 \
    --exec-tty --exec "$WORK/login.sh" \
    --max-call 120 \
    --log-file "$WORK/answer.log" \
    </dev/null >"$WORK/answer.out" 2>&1 &
ANS_PID=$!

for _ in $(seq 1 100); do
    grep -q "waiting for an inbound call" "$WORK/answer.log" 2>/dev/null && break
    sleep 0.2
done

wait_for() {
    for _ in $(seq 1 150); do
        LC_ALL=C grep -a -qF "$1" "$WORK/dial1.out" 2>/dev/null && return
        sleep 0.2
    done
}

fail=0
echo "==> call 1"
start=$(date +%s)
set +e
# Each answer waits for its prompt, since typeahead is echoed whatever the
# command later does, and ends with CR alone, as a terminal sends Enter.
"$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
    --server 127.0.0.1 --username call --no-register \
    --local-port "$CALL_PORT" --rtp-port "$CALL_RTP" \
    --modulation "$MODULATION" \
    --max-call 120 \
    --log-file "$WORK/dial1.log" \
    < <(wait_for 'name? '; printf 'alice\r'; wait_for 'secret? '; printf 'hunter2\r'; sleep 40) \
    >"$WORK/dial1.out"
rc=$?
set -e
took=$(( $(date +%s) - start ))
cat -v "$WORK/dial1.out"
for want in 'tty=\[yes\] size=\[24 80\] speed=\[300\]' 'name\? alice\r\r?$' 'secret\? \r?$' \
            'hello \[alice\] secret has 7 characters\r$'; do
    tr -d '\000' <"$WORK/dial1.out" | LC_ALL=C grep -a -qE "$(printf '%s' "$want" | sed 's/\\r/\r/g')" || {
        echo "==> FAIL: call 1: expected /$want/" >&2; fail=1; }
done
if LC_ALL=C grep -a -q hunter2 "$WORK/dial1.out"; then
    echo "==> FAIL: call 1: the secret was echoed with echo off" >&2; fail=1
fi
if [ "$took" -ge 35 ]; then
    echo "==> FAIL: call 1 took ${took}s; it should end when the command exits" >&2
    fail=1
fi
echo "==> call 1: dial exit $rc after ${took}s"

echo "==> call 2 (caller hangs up first)"
start=$(date +%s)
set +e
"$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
    --server 127.0.0.1 --username call --no-register \
    --local-port "$CALL_PORT" --rtp-port "$CALL_RTP" \
    --modulation "$MODULATION" \
    --hangup-on-eof \
    --max-call 120 \
    --log-file "$WORK/dial2.log" \
    < <(sleep 8) >"$WORK/dial2.out"
rc=$?
set -e
took=$(( $(date +%s) - start ))
echo "==> call 2: dial exit $rc after ${took}s"

for _ in $(seq 1 50); do
    kill -0 "$ANS_PID" 2>/dev/null || break
    sleep 0.2
done
if kill -0 "$ANS_PID" 2>/dev/null; then
    echo "==> FAIL: the answering modem is still running after --calls 2" >&2
    fail=1
else
    ans_rc=0
    wait "$ANS_PID" || ans_rc=$?
    ANS_PID=""
    echo "==> answering modem exited $ans_rc"
fi
grep -c 'exec.*finished' "$WORK/answer.log" | grep -qx 2 || {
    echo "==> FAIL: expected the command to have run and finished twice" >&2; fail=1; }
# Hung up on mid-read, the second must have been stopped by the hangup -
# the terminal's, or ours - rather than finishing by itself.
grep 'exec.*finished' "$WORK/answer.log" | tail -1 | grep -q 'signal 1' || {
    echo "==> FAIL: expected the second command to be ended by SIGHUP" >&2; fail=1; }
if grep -q 'did not exit after SIGHUP' "$WORK/answer.log"; then
    echo "==> FAIL: the second command had to be killed" >&2; fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "==> PASS"
    exit 0
fi
echo "--- answer.log (tail) ---" >&2
tail -40 "$WORK/answer.log" >&2 || true
exit 1
