#!/usr/bin/env python3
"""Drive `datamodem rlogin-listen` the way SyncTERM's RLogin connection does.

    scripts/rlogin-test.py [path/to/datamodem]

One datamodem answers on the loopback interface, handing each call to a small
echoing BBS. Another runs `datamodem rlogin-listen`, and this script is the
terminal program connecting to it - sending the RFC 1282 handshake as
SyncTERM does (src/syncterm/rlogin.c): a NUL, the entry's password, its user
name - here the number to dial - and the terminal type with its speed.

Then: RINGING and CONNECT on the connection, the far end's screen and every
byte value both ways, a second connection turned away while the call is up,
closing the connection hanging up, the number in the password field instead
(RLogin Reversed), a call nobody answers, a connection abandoned while it
dials, no number at all, and something that is not RLogin.
"""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/datamodem")
WORK = tempfile.mkdtemp(prefix="datamodem-rlogin")
ANS_PORT = os.environ.get("ANS_PORT", "5087")
CALL_PORT = os.environ.get("CALL_PORT", "5077")
ANS_RTP = os.environ.get("ANS_RTP", "5400")
CALL_RTP = os.environ.get("CALL_RTP", "5500")
RLOGIN_PORT = int(os.environ.get("RLOGIN_PORT", "5513"))
MODULATION = os.environ.get("MODULATION", "v22bis")
EXTRA = os.environ.get("DM_FLAGS", "").split()
FAR_END = f"sip:ans@127.0.0.1:{ANS_PORT}"
NOBODY = "sip:nobody@127.0.0.1:5999"

if not os.access(BIN, os.X_OK):
    sys.exit(f"no datamodem binary at {BIN} - run: cmake -S . -B build && cmake --build build")

failures = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        failures.append(msg)


def log_has(path, text):
    try:
        return text in open(path).read()
    except FileNotFoundError:
        return False


def wait_for_log(path, text, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if log_has(path, text):
            return True
        time.sleep(0.2)
    return False


SCREEN = (b"\x1b[2J\x1b[1;37;44m" + bytes(range(0xb0, 0xe0)) + b"\x1b[0m\r\n"
          + bytes(range(256)) + b"\r\nEND OF SCREEN\r\n")
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


class Client:
    """One SyncTERM directory entry's connection."""

    def __init__(self, user, password="", term="ansi-bbs/115200", handshake=True):
        self.sock = socket.create_connection(("127.0.0.1", RLOGIN_PORT), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.seen = bytearray()
        self.closed = threading.Event()
        if handshake:
            self.sock.sendall(b"\0" + password.encode() + b"\0" + user.encode() + b"\0"
                              + term.encode() + b"\0")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        self.sock.settimeout(None)
        while True:
            try:
                chunk = self.sock.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            self.seen.extend(chunk)
        self.closed.set()

    def expect(self, pattern, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if pattern in self.seen:
                return True
            time.sleep(0.05)
        return False

    def send(self, data):
        self.sock.sendall(data)

    def close(self):
        self.sock.close()


answerer = subprocess.Popen(
    [BIN, "answer", "--server", "127.0.0.1", "--username", "ans", "--no-register",
     "--local-port", ANS_PORT, "--rtp-port", ANS_RTP, "--modulation", MODULATION,
     "--calls", "0", "--max-call", "120",
     "--exec", f"exec {sys.executable} -u {WORK}/bbs.py",
     "--log-file", f"{WORK}/answer.log"] + EXTRA,
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
LOG = f"{WORK}/rlogin.log"
listener = subprocess.Popen(
    [BIN, "rlogin-listen", "--rlogin-port", str(RLOGIN_PORT),
     "--server", "127.0.0.1", "--username", "call", "--no-register",
     "--local-port", CALL_PORT, "--rtp-port", CALL_RTP, "--modulation", MODULATION,
     "--max-call", "120", "--connect-timeout", "5", "--log-file", LOG] + EXTRA,
    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    if not wait_for_log(f"{WORK}/answer.log", "waiting for an inbound call") or \
            not wait_for_log(LOG, "taking RLogin connections"):
        sys.exit("a datamodem never came up; see " + WORK)

    # An RLogin entry: the number in Username, a password that is not.
    c = Client(FAR_END, password="not-a-number")
    check(c.expect(b"\0", 5) and c.seen[:1] == b"\0", "the handshake was taken, with a NUL")
    check(c.expect(b"RINGING\r\n", 20), "RINGING, while it rang")
    check(c.expect(b"CONNECT ", 60), "CONNECT")
    check(c.expect(SCREEN, 20), "the far end's screen arrived byte for byte")
    check(b"[rlogin]" not in c.seen and b"[sip]" not in c.seen, "none of the log reached the connection")

    other = Client(FAR_END)
    check(other.closed.wait(10) and b"another call" in other.seen,
          "a second connection while the call is up was turned away")

    del c.seen[:]
    every = bytes(range(256)).replace(b"+", b"") + b"+"
    c.send(every)
    check(c.expect(every, 20), "all 256 byte values sent reached the far end and came back")

    # SyncTERM disconnecting closes the connection; that is the hangup.
    started = time.time()
    c.close()
    check(wait_for_log(f"{WORK}/answer.log", "the call was cleared by the far end", 10)
          and time.time() - started < 5,
          f"closing the connection hung up, in {time.time() - started:.1f}s")
    check(wait_for_log(LOG, "\"call\" cleared", 10) or wait_for_log(LOG, "cleared", 1),
          "and the listener cleared its side")
    time.sleep(1)

    # RLogin Reversed, or an address too long for Username: the password.
    c = Client("", password=FAR_END)
    check(c.expect(b"CONNECT ", 60) and c.expect(SCREEN, 20), "the number in the password field dials too")
    c.send(b"BYE")
    check(c.closed.wait(15) and c.seen.endswith(b"NO CARRIER\r\n"),
          "the far end hanging up says NO CARRIER and closes the connection")
    time.sleep(1)

    c = Client(NOBODY)
    check(c.closed.wait(30) and b"NO ANSWER\r\n" in c.seen, "a call nobody answers is NO ANSWER, then closed")

    c = Client(NOBODY)
    time.sleep(1.5)
    c.close()
    check(wait_for_log(LOG, "the call is abandoned", 5), "closing the connection while it dials abandons the call")
    time.sleep(1)

    c = Client("")
    check(c.closed.wait(10) and b"no number to dial" in c.seen, "no number at all is said so, then closed")

    c = Client("", handshake=False)
    c.send(b"GET / HTTP/1.0\r\n\r\n")
    check(c.closed.wait(15) and b"CONNECT" not in c.seen, "something that is not RLogin is closed")

    c = Client(FAR_END)
    check(c.expect(b"CONNECT ", 60), "and after all that, the next connection still dials")
    c.close()
    time.sleep(2)
finally:
    listener.send_signal(signal.SIGTERM)
    try:
        rc = listener.wait(timeout=10)
    except subprocess.TimeoutExpired:
        listener.kill()
        rc = None
    check(rc == 0, f"SIGTERM stopped the listener (exit {rc})")
    answerer.send_signal(signal.SIGTERM)
    try:
        answerer.wait(timeout=20)
    except subprocess.TimeoutExpired:
        answerer.kill()

print("PASS" if not failures else f"FAIL ({len(failures)} checks)")
print("logs and output in", WORK)
sys.exit(0 if not failures else 1)
