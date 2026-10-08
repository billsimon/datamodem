#!/usr/bin/env bash
# Stepping down, over the selftest's simulated line: a modem left at its
# defaults - V.34, stepping down - against every older modem it might meet,
# calling and answering, and two that both step down from different places;
# then V.42 and V.42bis, on by default, against far ends that will not
# compress or do not do V.42 at all (and none of it at V.21 or Bell 103). The older modem is datamodem with
# --no-step-down, which behaves as one: a V.22 bis answerer that sends USB1
# and waits, a V.21 caller that says nothing until it hears channel 2.
#
#   scripts/step-down-test.sh [path/to/datamodem]
#
# Each case prints what the two ended up running and whether the data came
# through intact. Takes a couple of minutes.
set -uo pipefail

BIN=${1:-build/datamodem}
[ -x "$BIN" ] || { echo "no datamodem binary at $BIN" >&2; exit 1; }

pass=0
fail=0

# case NAME EXPECT FAR LINE [datamodem selftest arguments]
#   EXPECT  the modulation both should end up on, and with V42= set, the
#           protocol too: modulation/protocol
#   FAR     DATAMODEM_SELFTEST_FAR: which end is the older modem, and what it is
case_() {
    local name=$1 expect=$2 far=$3 line=$4; shift 4
    local out rc got proto result
    out=$(env DATAMODEM_SELFTEST_FAR=$far DATAMODEM_SELFTEST_V42=${V42:-} DATAMODEM_SELFTEST_LINE=$line \
          ${ANSAM:+DATAMODEM_ANSWER_ANSAM=1} \
          "$BIN" selftest "$@" 2>&1)
    rc=$?
    got=$(printf '%s\n' "$out" | grep -oE "result modulation=[^ ]+" | head -1)
    got=${got#result modulation=}
    proto=$(printf '%s\n' "$out" | grep -oE "result modulation=[^ ]+ rate=[0-9]+ protocol=[^ ]+" | head -1)
    proto=${proto##*protocol=}
    [ "$expect" = "${expect%/*}" ] || got="$got/$proto"
    result=$(printf '%s\n' "$out" | grep -oE "rate=[0-9]+ protocol=[^ ]+ link_seconds=[0-9.-]+" | head -1)
    if [ $rc -eq 0 ] && [ "$got" = "$expect" ]; then
        pass=$((pass + 1))
        printf 'PASS  %-44s %-8s %s\n' "$name" "$got" "$result"
    else
        fail=$((fail + 1))
        printf 'FAIL  %-44s exit %d, %s (wanted %s)\n' "$name" "$rc" "${got:-nothing}" "$expect"
        printf '%s\n' "$out" | grep -E "error|warn|modulation tag|settled" | tail -6 | sed 's/^/        /'
    fi
}

for line in "" "ulaw,delay=150,echo=-12" "ulaw,delay=350,noise=-40"; do
    for v42 in "--v42 off" ""; do
        tag="${line:-perfect}${v42:+, $v42}"
        echo "== $tag"
        # The answering end is the older modem; ours calls.
        case_ "calls a V.34 modem"          v34     answer:v34     "$line" $v42
        case_ "calls a V.32 bis modem"      v32bis  answer:v32bis  "$line" $v42
        case_ "calls a V.32 modem"          v32bis  answer:v32     "$line" $v42
        case_ "calls a V.22 bis modem"      v22bis  answer:v22bis  "$line" $v42
        case_ "calls a V.22 modem"          v22bis  answer:v22     "$line" $v42
        case_ "calls a V.21 modem"          v21     answer:v21     "$line" $v42
        case_ "calls a Bell 103 modem"      bell103 answer:bell103 "$line" $v42
        # And the other way round.
        case_ "answers a V.34 modem"        v34     call:v34       "$line" $v42
        case_ "answers a V.32 bis modem"    v32bis  call:v32bis    "$line" $v42
        case_ "answers a V.32 modem"        v32     call:v32       "$line" $v42
        case_ "answers a V.22 bis modem"    v22bis  call:v22bis    "$line" $v42
        case_ "answers a V.22 modem"        v22     call:v22       "$line" $v42
        case_ "answers a V.21 modem"        v21     call:v21       "$line" $v42
        case_ "answers a Bell 103 modem"    bell103 call:bell103   "$line" $v42
    done
done

echo "== both stepping down, from different places"
case_ "v22bis caller, v32bis answerer"      v22bis  answer:v32bis:auto ""  --modulation v22bis
case_ "v22bis caller, v32 answerer"         v22bis  call:v22bis:auto   ""  --modulation v32
case_ "v21 caller, v34 answerer"            v21     answer:v34:auto    ""  --modulation v21
case_ "v34 caller, v21 answerer"            v21     answer:v21:auto    ""  --modulation v34
case_ "bell103 caller, v34 answerer"        bell103 answer:v34:auto    ""  --modulation bell103
case_ "v34 caller, v22 answerer"            v22bis  call:v34:auto      ""  --modulation v22
case_ "--bit-rate 9600, a V.22 bis answerer" v22bis answer:v22bis      ""  --bit-rate 9600

echo "== an older answerer whose answer tone is taken for ANSam: V.8 goes unanswered"
for line in "" "ulaw,delay=150,echo=-12" "ulaw,delay=350,noise=-40"; do
    for mod in v32bis v32 v22bis v22 v21; do
        want=$mod; case $mod in v32) want=v32bis ;; v22) want=v22bis ;; esac
        ANSAM=1 case_ "${line:-perfect}: calls a $mod modem" "$want" "answer:$mod" "$line"
    done
    # A 2400 bps modem met on a real call: answer tone (heard as ANSam),
    # USB1 for three seconds, then on to V.21 - which is what our own
    # answerer does when it starts at v22bis.
    ANSAM=1 case_ "${line:-perfect}: 2400 modem, USB1 then V.21" v22bis "answer:v22bis:auto" "$line"
done

echo "== V.42 and V.42bis by default, and what each falls back to"
# V.21 and Bell 103 run no V.42 at all, whatever either end asks for.
for line in "" "ulaw,delay=150,echo=-12"; do
    for mod in v34 v32bis v22bis v21 bell103; do
        far="answer:$mod"
        both="$mod/V.42/V.42bis" ec="$mod/V.42"
        case $mod in v21|bell103) both="$mod/async" ec="$mod/async" ;; esac
        case_ "${line:-perfect}: $mod, both ends default"        "$both"             "$far" "$line"
        V42=answer:no-v42bis \
        case_ "${line:-perfect}: $mod, answerer will not compress" "$ec"             "$far" "$line"
        V42=call:no-v42bis \
        case_ "${line:-perfect}: $mod, caller will not compress"   "$ec"             "$far" "$line"
        V42=answer:off \
        case_ "${line:-perfect}: $mod, answerer has no V.42"       "$mod/async"      "$far" "$line"
        V42=call:off \
        case_ "${line:-perfect}: $mod, caller has no V.42"         "$mod/async"      "$far" "$line"
    done
done

echo
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
