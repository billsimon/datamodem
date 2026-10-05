#!/usr/bin/env python3
"""The full-screen terminal against a hostile far end.

    scripts/screen-test.py [path/to/datamodem]

One datamodem answers and sends a BBS screen - colour, CP437 boxes, a cursor
position query - followed by everything that could damage a real terminal:
cursor movement onto the status line, scroll regions, the alternate screen,
mouse reporting, a title, character set switches, a full reset, erasing
below the cursor, and 3 KB of random bytes. Another dials it on a pty, with
the screen ours, and everything it writes to that pty is checked: none of the
far end's sequences may reach it, and replayed through a terminal emulator
the screen has to show the far end's text above an intact status line, and
hand the shell a fresh line below that line at the end.

Needs pyte (pip install pyte), a terminal emulator in Python, to see the
screen; skips without it.
"""
import fcntl
import os
import pty
import random
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

try:
    import pyte
except ImportError:
    print("SKIP: needs pyte (pip install pyte)")
    sys.exit(0)

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/datamodem")
W, H = 80, 24
MODULATION = os.environ.get("MODULATION", "v32bis")
work = tempfile.mkdtemp(prefix="datamodem-screen")

random.seed(7)
box = bytes([0xC9]) + bytes([0xCD]) * 30 + bytes([0xBB])
mid = bytes([0xBA]) + b"  WELCOME TO THE TEST BBS     " + bytes([0xBA])
bot = bytes([0xC8]) + bytes([0xCD]) * 30 + bytes([0xBC])
hostile = (
    b"\x1b[2J\x1b[H"
    b"\x1b[1;33;44m" + box + b"\r\n" + mid + b"\r\n" + bot + b"\x1b[0m\r\n"
    b"\x1b[6n"                      # where is the cursor?
    b"\x1b[99;1HOVERWRITE-STATUS"   # off the bottom of the screen
    b"\x1b[r\x1b[1;24r"             # scroll regions
    b"\x1b[?1049h\x1b[?1000h"       # alternate screen, mouse reporting
    b"\x1b]0;HACKED TITLE\x07"       # window title
    b"\x0e\x1b(0"                    # character set switches
    b"\x1b[5i"                       # printer on
    b"\x1bc"                         # full reset
    b"\x1b[J"                        # erase below: the status line, on a real terminal
)
noise = bytes(random.getrandbits(8) for _ in range(3000))
lines = b"".join(b"line %03d of the scroll test\r\n" % i for i in range(60))
payload = hostile + b"\r\n" + noise + b"\r\n\x1b[0m" + lines + b"END OF PAYLOAD\r\n"
with open(f"{work}/payload", "wb") as f:
    f.write(payload)

ans = subprocess.Popen(
    [BIN, "answer", "--server", "127.0.0.1", "--username", "ans", "--no-register", "--local-port", "5080",
     "--rtp-port", "4100", "--modulation", MODULATION, "--idle-timeout", "12", "--max-call", "120",
     "--log-file", f"{work}/answer.log"],
    stdin=open(f"{work}/payload", "rb"), stdout=open(f"{work}/answer.out", "wb"), stderr=subprocess.DEVNULL)
time.sleep(1.5)

pid, fd = pty.fork()
if pid == 0:
    os.environ.setdefault("LANG", "en_US.UTF-8")
    os.environ["LC_ALL"] = "en_US.UTF-8"
    os.execv(BIN, [BIN, "sip:ans@127.0.0.1:5080", "--server", "127.0.0.1", "--username", "call", "--no-register",
                   "--local-port", "5070", "--rtp-port", "4200", "--modulation", MODULATION, "--idle-timeout",
                   "10", "--max-call", "120", "--log-file", f"{work}/dial.log"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", H, W, 0, 0))
out = bytearray()
start = time.time()
while time.time() - start < 150:
    r, _, _ = select.select([fd], [], [], 0.2)
    if r:
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        out += data
else:
    os.kill(pid, 9)
os.waitpid(pid, 0)
ans.wait()
with open(f"{work}/screen.raw", "wb") as f:
    f.write(out)

failures = 0


def check(ok, label):
    global failures
    print(("  ok   " if ok else "  FAIL ") + label)
    if not ok:
        failures += 1


forbidden = {b"\x1b]": "a window title", b"\x1b(0": "a character set switch", b"\x0e": "shift out",
             b"\x1b[?1049": "the alternate screen", b"\x1b[?1000": "mouse reporting", b"\x1bc": "a full reset",
             b"\x1b[5i": "the printer", b"\x1b[1;24r": "a scroll region over the status line"}
for seq, name in forbidden.items():
    check(seq not in out, f"{name} did not reach the terminal")

# The screen as it was in use: everything up to the hand-back.
handback = out.rfind(b"\x1b[r")
screen = pyte.Screen(W, H)
pyte.ByteStream(screen).feed(bytes(out[:handback] if handback > 0 else out))
rows = screen.display
status = rows[-1]
text = "\n".join(rows)
check("OFFLINE" in status, "the status line is on the bottom row, and says the call is over")
check(screen.buffer[H - 1][2].reverse, "the status line is in reverse video")
check("OVERWRITE" not in text, "the far end could not write on the status line")
check("END OF PAYLOAD" in text, "the far end's last line is on the screen")
check("╔" in out.decode("utf-8", "replace"), "CP437 box drawing came out as Unicode")

after = pyte.Screen(W, H)
pyte.ByteStream(after).feed(bytes(out))
check("OFFLINE" in after.display[-2] and after.display[-1].strip() == "",
      "the shell gets a fresh line below the status line")
check(after.margins is None, "the scroll region was given back")
with open(f"{work}/answer.out", "rb") as f:
    check(f.read().startswith(b"\x1b[") , "the far end's cursor position query was answered")

if failures:
    print(f"FAIL ({failures} checks); output in {work}")
    sys.exit(1)
print(f"PASS; output in {work}")
