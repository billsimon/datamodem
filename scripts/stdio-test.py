#!/usr/bin/env python3
"""Drive `datamodem --stdio` the way a terminal program runs it.

    scripts/stdio-test.py [path/to/datamodem]

SyncTERM's Shell connection on Linux and macOS runs a command on a pty and
draws whatever it writes; that is what this does with `datamodem <number>
--stdio`, against a datamodem answering on the loopback interface for a BBS
that sends ANSI, an escape sequence that would retitle a real terminal, and
every byte value there is.

All of it has to reach the pty exactly as sent - no full screen, no
filtering - with RINGING and CONNECT ahead of it and none of the log. What is
typed has to reach the far end exactly too, ctrl-C and all; SIGHUP, which is
how SyncTERM disconnects, has to hang up promptly; and a call nobody answers
has to say NO ANSWER.
"""
import os
import pty
import signal
import subprocess
import sys
import tempfile
import termios
import threading
import time
import select

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/datamodem")
WORK = tempfile.mkdtemp(prefix="datamodem-stdio")
ANS_PORT = os.environ.get("ANS_PORT", "5088")
CALL_PORT = os.environ.get("CALL_PORT", "5078")
ANS_RTP = os.environ.get("ANS_RTP", "5200")
CALL_RTP = os.environ.get("CALL_RTP", "5300")
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


# What the far end sends first: a BBS screen's worth of ANSI and CP437, an
# OSC that would set a real terminal's title, and then every byte value.
SCREEN = (b"\x1b[2J\x1b[1;37;44m" + bytes(range(0xb0, 0xe0)) + b"\x1b[0m\r\n"
          + b"\x1b]0;retitled\x07" + bytes(range(256)) + b"\r\nEND OF SCREEN\r\n")
open(f"{WORK}/screen.bin", "wb").write(SCREEN)
BBS = (
    "import os,sys\n"
    f"os.write(1, open({WORK + '/screen.bin'!r}, 'rb').read())\n"
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


def run_caller(target, extra=()):
    """datamodem on a pty - stdin, stdout and stderr - as SyncTERM runs a
    Shell entry's command, and with no --log-file, so that anything it logs
    where it should not lands on the pty and is caught."""
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        [BIN, target, "--stdio", "--server", "127.0.0.1", "--username", "call", "--no-register",
         "--local-port", CALL_PORT, "--rtp-port", CALL_RTP, "--modulation", MODULATION,
         "--max-call", "120"] + list(extra) + EXTRA,
        stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    seen = bytearray()
    state = {"reading": True}

    def drain():
        while state["reading"]:
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                chunk = os.read(master, 4096)
            except OSError:
                return
            if not chunk:
                return
            seen.extend(chunk)

    threading.Thread(target=drain, daemon=True).start()
    return proc, master, seen, state


def expect(seen, pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pattern in seen:
            return True
        time.sleep(0.05)
    return False


caller = None
try:
    caller, master, seen, state = run_caller(f"sip:ans@127.0.0.1:{ANS_PORT}")
    check(expect(seen, b"RINGING\r\n", 20), "RINGING, while it rang")
    check(expect(seen, b"CONNECT ", 60), "CONNECT")
    check(expect(seen, SCREEN, 20), "the far end's screen arrived byte for byte, escape sequences and all")
    text = bytes(seen)
    check(b"info " not in text and b"[sip]" not in text and b"[modem]" not in text,
          "none of the log reached the line")
    check(b"\x1b[?1049h" not in text and b"ONLINE" not in text, "no full screen, no status line")

    attrs = termios.tcgetattr(master)
    check(not attrs[3] & termios.ECHO and not attrs[3] & termios.ICANON and not attrs[3] & termios.ISIG
          and not attrs[1] & termios.OPOST, "the pty is raw while connected")

    del seen[:]
    every = bytes(range(256)).replace(b"+", b"") + b"+"
    os.write(master, every)
    check(expect(seen, every, 20), "all 256 byte values typed reached the far end and came back, ctrl-C too")

    # SyncTERM disconnecting: it sends the command SIGHUP, and SIGKILL ten
    # seconds later. The call has to be cleared well inside that.
    os.killpg(caller.pid, signal.SIGHUP)
    started = time.time()
    try:
        rc = caller.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rc = None
    check(rc is not None and time.time() - started < 5,
          f"SIGHUP hung up and exited in {time.time() - started:.1f}s (exit {rc})")
    check(wait_for_log(f"{WORK}/answer.log", "the call was cleared by the far end", 10),
          "the far end saw the call cleared")
    state["reading"] = False
    os.close(master)

    # A call that nobody answers says so, on the line, as a modem would.
    caller, master, seen, state = run_caller("sip:nobody@127.0.0.1:5999", ["--connect-timeout", "5"])
    try:
        rc = caller.wait(timeout=30)
    except subprocess.TimeoutExpired:
        rc = None
    time.sleep(0.3)
    check(b"NO ANSWER\r\n" in seen, f"a call nobody answers is NO ANSWER (exit {rc})")
    check(b"[dial]" not in seen or b"error" in seen, "with only errors from the log, if anything")
    state["reading"] = False
    os.close(master)
finally:
    if caller is not None and caller.poll() is None:
        caller.kill()
    answerer.send_signal(signal.SIGTERM)
    try:
        answerer.wait(timeout=20)
    except subprocess.TimeoutExpired:
        answerer.kill()

print("PASS" if not failures else f"FAIL ({len(failures)} checks)")
print("logs and output in", WORK)
sys.exit(0 if not failures else 1)
