#!/usr/bin/env python3
"""Drive datamodem through a real pty over a real call.

This is the only test that exercises raw mode: the shell tests all pipe their
input, so isatty() is false and the terminal handling never runs. Here both
stdin and stdout are a pty, which is what an actual user has.

    scripts/pty-test.py [path/to/datamodem]

Checks CONNECT, that the terminal really is in raw mode while connected, the
+++ escape twice over with ATI and ATO in between, ATH, that the terminal is
handed back the way it was found, and that the three plus signs never reached
the far end.
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

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/datamodem")
WORK = tempfile.mkdtemp(prefix="datamodem-pty")
ANS_PORT = os.environ.get("ANS_PORT", "5082")
CALL_PORT = os.environ.get("CALL_PORT", "5072")
ANS_RTP = os.environ.get("ANS_RTP", "4500")
CALL_RTP = os.environ.get("CALL_RTP", "4600")
MODULATION = os.environ.get("MODULATION", "v21")
# Extra flags for both ends, e.g. DM_FLAGS="--v42 detect --v42bis" to drive
# the same interactive session over an error-corrected, compressed link.
EXTRA = os.environ.get("DM_FLAGS", "").split()

if not os.access(BIN, os.X_OK):
    sys.exit(f"no datamodem binary at {BIN} - run: cmake -S . -B build && cmake --build build")

failures = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        failures.append(msg)


answerer = subprocess.Popen(
    [BIN, "answer", "--server", "127.0.0.1", "--username", "ans", "--no-register",
     "--local-port", ANS_PORT, "--rtp-port", ANS_RTP, "--modulation", MODULATION,
     "--max-call", "120", "--log-file", f"{WORK}/answer.log"] + EXTRA,
    stdin=subprocess.DEVNULL, stdout=open(f"{WORK}/answer.out", "wb"),
    stderr=subprocess.DEVNULL)

for _ in range(150):
    try:
        if "waiting for an inbound call" in open(f"{WORK}/answer.log").read():
            break
    except FileNotFoundError:
        pass
    time.sleep(0.2)

master, slave = pty.openpty()
original = termios.tcgetattr(slave)

caller = subprocess.Popen(
    [BIN, f"sip:ans@127.0.0.1:{ANS_PORT}", "--server", "127.0.0.1", "--username", "call",
     "--no-register", "--local-port", CALL_PORT, "--rtp-port", CALL_RTP,
     "--modulation", MODULATION, "--max-call", "120", "--log-file", f"{WORK}/dial.log"] + EXTRA,
    stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)

# The pty has to be drained continuously and from its own thread. If nothing
# reads it, datamodem eventually blocks writing to the terminal - which is
# ordinary behaviour for any program, but looks exactly like a hang if the
# test only reads when it happens to be waiting for something.
seen = bytearray()
reading = True


def drain():
    while reading:
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


def expect(pattern, timeout, label):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if re.search(pattern, seen.decode("utf-8", "replace")):
            check(True, label)
            return True
        time.sleep(0.1)
    check(False, label)
    return False


def forget():
    del seen[:]


try:
    # The rate depends on the modulation, so match whatever it reports.
    expect(r"CONNECT \d+", 90, "CONNECT appeared on the terminal")
    if "--v42bis" in EXTRA:
        expect(r"CONNECT \d+ V\.42/V\.42bis", 5, "CONNECT named the protocol")

    attrs = termios.tcgetattr(master)
    check(not attrs[3] & termios.ECHO, "ECHO is off while connected")
    check(not attrs[3] & termios.ICANON, "ICANON is off while connected")
    check(not attrs[3] & termios.ISIG, "ISIG is off, so ctrl-C reaches the far end")
    check(not attrs[1] & termios.OPOST, "OPOST is off, so the far end's CR/LF is not rewritten")

    os.write(master, b"hello\r")
    time.sleep(2.0)                      # the leading guard time
    forget()
    os.write(master, b"+++")
    expect(r"OK", 8, "+++ reached command mode")

    forget()
    os.write(master, b"ATI\r")
    expect(r"modulation\s+%s" % MODULATION, 5, "ATI reported the session")
    if "--v42bis" in EXTRA:
        expect(r"protocol\s+V\.42/V\.42bis", 5, "ATI reported V.42/V.42bis")
        expect(r"sent\s+\d+ bytes, \d+ on the wire \([\d.]+:1\)", 5,
               "ATI accounted for what compression cost on the line")
    elif "--v42" in EXTRA:
        expect(r"protocol\s+V\.42", 5, "ATI reported V.42")

    forget()
    os.write(master, b"ATO\r")
    expect(r"CONNECT \d+", 5, "ATO went back online")

    time.sleep(2.0)
    forget()
    os.write(master, b"+++")
    expect(r"OK", 8, "+++ worked a second time")

    os.write(master, b"ATH\r")
    try:
        rc = caller.wait(timeout=30)
    except subprocess.TimeoutExpired:
        caller.kill()
        rc = None
    check(rc == 0, f"ATH hung up and exited cleanly (exit {rc})")

    restored = termios.tcgetattr(master)
    check(restored[3] & termios.ECHO, "the terminal was handed back with ECHO on")
    check(restored[3] & termios.ICANON, "the terminal was handed back with ICANON on")
    check(restored[3] & termios.ISIG, "the terminal was handed back with ISIG on")
finally:
    reading = False
    if caller.poll() is None:
        caller.kill()
    answerer.send_signal(signal.SIGTERM)
    try:
        answerer.wait(timeout=20)
    except subprocess.TimeoutExpired:
        answerer.kill()

received = open(f"{WORK}/answer.out", "rb").read()
check(b"hello" in received, "the data reached the far end")
check(b"+++" not in received, "+++ was held back and never went out on the line")

print("PASS" if not failures else f"FAIL ({len(failures)} checks)")
print("logs and output in", WORK)
sys.exit(0 if not failures else 1)
