#!/usr/bin/env bash
# End-to-end test of what an answering modem says first on a direct async
# link: one datamodem answers with --v42 off and --exec a "BBS" that prints
# its banner the moment it starts, another dials it, and the banner must
# reach the caller whole - its first character included.
#
#   scripts/async-answer-test.sh [path/to/datamodem]
#
# Over V.42 nothing said early can be lost, since LAPM is only up once both
# ends are. Async has nothing to resend with, and the two ends do not
# connect together: on FSK the answering one connects on hearing the
# caller's carrier, the calling one half a second later, and V.22 and
# V.22bis callers finish training well after the answerer. MODULATIONS picks
# which to try; the default is every one.
set -euo pipefail

BIN=${1:-build/datamodem}
WORK=$(mktemp -d -t datamodem-async-answer)
ANS_PORT=${ANS_PORT:-5280}
CALL_PORT=${CALL_PORT:-5270}
ANS_RTP=${ANS_RTP:-4500}
CALL_RTP=${CALL_RTP:-4600}
MODULATIONS=${MODULATIONS:-"v21 bell103 v23 v22 v22bis v32 v32bis v34"}

cleanup() {
    [ -n "${ANS_PID:-}" ] && kill "$ANS_PID" 2>/dev/null || true
    wait "${ANS_PID:-}" 2>/dev/null || true
    echo "logs and output in $WORK"
}
trap cleanup EXIT

[ -x "$BIN" ] || { echo "no datamodem binary at $BIN" >&2; exit 1; }

# Says its piece at once, as a BBS does, and leaves: the call ends when the
# line has carried it.
cat >"$WORK/bbs.sh" <<'EOF'
#!/bin/sh
printf 'WELCOME TO THE TEST BBS\r\nline 1 of the banner\r\nline 2 of the banner\r\n'
EOF
chmod +x "$WORK/bbs.sh"

fail=0
for mod in $MODULATIONS; do
    echo "==> $mod"
    "$BIN" answer \
        --server 127.0.0.1 --username ans --no-register \
        --local-port "$ANS_PORT" --rtp-port "$ANS_RTP" \
        --modulation "$mod" --v42 off \
        --calls 1 \
        --exec "$WORK/bbs.sh" \
        --max-call 60 \
        --log-file "$WORK/answer-$mod.log" \
        </dev/null >"$WORK/answer-$mod.out" 2>&1 &
    ANS_PID=$!

    for _ in $(seq 1 100); do
        grep -q "waiting for an inbound call" "$WORK/answer-$mod.log" 2>/dev/null && break
        sleep 0.2
    done

    set +e
    "$BIN" "sip:ans@127.0.0.1:$ANS_PORT" \
        --server 127.0.0.1 --username call --no-register \
        --local-port "$CALL_PORT" --rtp-port "$CALL_RTP" \
        --modulation "$mod" --v42 off \
        --max-call 60 \
        --log-file "$WORK/dial-$mod.log" \
        < <(sleep 40) >"$WORK/dial-$mod.out"
    rc=$?
    set -e
    wait "$ANS_PID" 2>/dev/null || true
    ANS_PID=""

    cat -v "$WORK/dial-$mod.out"
    # The banner's first line whole, not its tail: what goes missing is
    # whatever the answering end said before the caller was listening.
    if LC_ALL=C grep -a -qE "^WELCOME TO THE TEST BBS.?$" "$WORK/dial-$mod.out" &&
        LC_ALL=C grep -a -qF "line 2 of the banner" "$WORK/dial-$mod.out"; then
        echo "==> $mod: the banner arrived whole (dial exit $rc)"
    else
        echo "==> FAIL: $mod: the caller did not receive the banner whole (dial exit $rc)" >&2
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "==> PASS"
    exit 0
fi
exit 1
