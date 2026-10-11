#!/usr/bin/env python3
"""Drive `datamodem modem` the way a terminal program drives a modem.

    scripts/modem-test.py [path/to/datamodem]

One datamodem answers on the loopback interface, handing each call to a small
echoing BBS. Another runs `datamodem modem` on a pty, and this script is the
terminal program at the other end of it - talking to it the way SyncTERM's
Modem connection does (src/syncterm/modem.c): an init string, OK, ATDT and the
number, CONNECT within 60 seconds, skipping the modem's echo of each command.

Then: the AT command set an init string written for some other modem leans
on, numeric result codes, a call carrying every byte value both ways, +++ and
ATH, ATDL, the far end hanging up, a dial abandoned by a keystroke, and the
port going away.

A pty has no modem-control lines, so DCD and DTR are not covered here.
"""
import os
import pty
import re
import select
import signal
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/datamodem")
WORK = tempfile.mkdtemp(prefix="datamodem-modem")
ANS_PORT = os.environ.get("ANS_PORT", "5086")
CALL_PORT = os.environ.get("CALL_PORT", "5076")
ANS_RTP = os.environ.get("ANS_RTP", "4900")
CALL_RTP = os.environ.get("CALL_RTP", "5000")
MODULATION = os.environ.get("MODULATION", "v22bis")
EXTRA = os.environ.get("DM_FLAGS", "").split()

if not os.access(BIN, os.X_OK):
    sys.exit(f"no datamodem binary at {BIN} - run: cmake -S . -B build && cmake --build build")

failures = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        failures.append(msg)


def wait_for_log(path, text, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if text in open(path).read():
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


# The far end: a BBS that greets the caller, echoes what it is sent, and
# hangs up when it is sent BYE.
BBS = (
    "import os,sys\n"
    "os.write(1, b'WELCOME TO THE TEST BBS\\r\\n')\n"
    "while True:\n"
    "    d = os.read(0, 4096)\n"
    "    if not d or b'BYE' in d: break\n"
    "    os.write(1, d)\n"
)
open(f"{WORK}/bbs.py", "w").write(BBS)

answerer = subprocess.Popen(
    [BIN, "answer", "--server", "127.0.0.1", "--username", "ans", "--no-register",
     "--local-port", ANS_PORT, "--rtp-port", ANS_RTP, "--modulation", MODULATION,
     "--calls", "0", "--max-call", "120",
     "--exec", f"exec {sys.executable} -u {WORK}/bbs.py",
     "--log-file", f"{WORK}/answer.log"] + EXTRA,
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if not wait_for_log(f"{WORK}/answer.log", "waiting for an inbound call"):
    answerer.kill()
    sys.exit("the answering datamodem never came up; see " + WORK)

# The port: datamodem opens the pty's slave as if it were /dev/ttyS0, and
# this script holds the master, as the terminal program at the other end of
# a null-modem cable would.
master, slave = pty.openpty()
port = os.ttyname(slave)
tty.setraw(master)
modem = subprocess.Popen(
    [BIN, "modem", "--port", port, "--server", f"127.0.0.1:{ANS_PORT}", "--username", "call",
     "--no-register", "--local-port", CALL_PORT, "--rtp-port", CALL_RTP,
     "--modulation", MODULATION, "--connect-timeout", "15",
     "--log-file", f"{WORK}/modem.log"] + EXTRA,
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(f"{WORK}/modem.err", "wb"))
os.close(slave)

received = bytearray()
lock = threading.Lock()
reading = True


def drain():
    while reading:
        r, _, _ = select.select([master], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(master, 4096)
        except OSError:
            chunk = b""
        # macOS reads end-of-file from a pty's master until something has
        # opened the slave, so that is not the end of anything yet.
        if not chunk:
            time.sleep(0.05)
            continue
        with lock:
            received.extend(chunk)


threading.Thread(target=drain, daemon=True).start()


def take(n=None):
    with lock:
        if n is None:
            n = len(received)
        out = bytes(received[:n])
        del received[:n]
    return out


def response(timeout, sent=None):
    """One line from the modem, as SyncTERM's modem_response() reads one:
    control characters before it are skipped, and it ends at a CR. A line
    containing what was just sent is the modem's echo of it, and skipped,
    as modem_connect() does."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        with lock:
            while received and received[0] < 0x20:
                del received[0]
            cr = received.find(b"\r")
            if cr >= 0:
                line = bytes(received[:cr]).decode("latin1")
                del received[:cr + 1]
                if sent is not None and sent in line:
                    continue
                return line
        time.sleep(0.05)
    return None


def command(cmd, timeout=5):
    take()
    os.write(master, cmd.encode() + b"\r")
    return response(timeout, sent=cmd)


def lines_until(final, timeout=5):
    out = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = response(deadline - time.time())
        if line is None:
            break
        out.append(line)
        if line == final:
            break
    return out


def expect_bytes(pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with lock:
            if pattern in received:
                return True
        time.sleep(0.05)
    return False


try:
    check(wait_for_log(f"{WORK}/modem.log", "a modem on"), "datamodem modem opened the port")

    # --- what SyncTERM sends: its default init string, then ATDT
    check(command("AT&F&C1&D2") == "OK", "SyncTERM's default init string: OK")
    check(command("ATZ E1 Q0 V1 X4 &K0 \\N3 %C1 S0=0 M1 L2 &S0 +MS=V34;") == "OK",
          "an init string for some other modem: OK")
    check(command("ATG") == "ERROR", "a command it does not know: ERROR")
    check(command("ATS12=50") == "ERROR", "S12 below 100 ms: ERROR")

    take()
    os.write(master, b"ATI\r")
    got = lines_until("OK")
    check(any(re.match(r"datamodem \d", l) for l in got) and got[-1:] == ["OK"],
          "ATI names it, then OK")
    take()
    os.write(master, b"ATS2?\r")
    got = lines_until("OK")
    check("043" in got, "ATS2? reads back 043, the plus sign")

    os.write(master, b"ATV0\r")
    time.sleep(0.5)
    take()
    os.write(master, b"AT\r")
    time.sleep(0.5)
    check(take() == b"AT\r0\r", "ATV0 answers in numbers: 0 for OK")
    os.write(master, b"ATE0V1\r")
    time.sleep(0.5)
    take()
    os.write(master, b"AT\r")
    time.sleep(0.5)
    check(take() == b"\r\nOK\r\n", "ATE0 stops the echo; V1 words it again, framed CR LF")
    check(command("ATE1") == "OK", "ATE1")

    # --- a call
    started = time.time()
    first = command("ATDT 555-1234", timeout=60)
    check(first is not None and first.startswith("CONNECT"),
          f"ATDT 555-1234: {first} after {time.time() - started:.1f}s (SyncTERM allows 60)")
    check("sip:5551234@" in open(f"{WORK}/modem.log").read(),
          "the dashes and spaces were dropped from the number")
    check(expect_bytes(b"WELCOME TO THE TEST BBS", 15), "the far end's greeting arrived")

    take()
    every = bytes(range(256)).replace(b"+", b"") + b"+"
    os.write(master, every)
    check(expect_bytes(every, 20), "all 256 byte values went there and back, untouched")

    time.sleep(1.5)
    take()
    os.write(master, b"+++")
    check(response(5) == "OK", "+++ escaped to command mode")
    check(command("ATH") == "OK", "ATH hung up")

    # --- ATDL, and the far end hanging up. The answering end takes a moment
    # to be ready for another call, and until it is, it is busy.
    check(wait_for_log(f"{WORK}/answer.log", "ready for the next call"),
          "the answering end is ready for another call")
    again = command("ATDL", timeout=60)
    check(again is not None and again.startswith("CONNECT"), f"ATDL dialled it again: {again}")
    check(expect_bytes(b"WELCOME TO THE TEST BBS", 15), "and it answered again")
    take()
    os.write(master, b"BYE\r")
    check(response(30) == "NO CARRIER", "the far end hanging up is NO CARRIER")
    check(command("AT") == "OK", "and it is back in command mode")

    # --- a dial abandoned by a keystroke, as on any Hayes
    take()
    os.write(master, b"ATDTsip:nobody@127.0.0.1:5999\r")
    time.sleep(1.5)
    os.write(master, b"x")
    started = time.time()
    got = response(10, sent="ATDT")
    check(got == "NO CARRIER" and time.time() - started < 5,
          f"a keystroke while it rings abandons the call: {got}")

    # --- one nobody answers
    got = command("ATDTsip:nobody@127.0.0.1:5999", timeout=40)
    check(got in ("NO ANSWER", "NO CARRIER", "NO DIALTONE"), f"a call that fails says so: {got}")

    # --- the port going away
    reading = False
    time.sleep(0.3)
    os.close(master)
    try:
        rc = modem.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rc = None
    check(rc == 3, f"the port going away ends it (exit {rc})")
finally:
    reading = False
    if modem.poll() is None:
        modem.kill()
    answerer.send_signal(signal.SIGTERM)
    try:
        answerer.wait(timeout=20)
    except subprocess.TimeoutExpired:
        answerer.kill()

print("PASS" if not failures else f"FAIL ({len(failures)} checks)")
print("logs and output in", WORK)
sys.exit(0 if not failures else 1)
