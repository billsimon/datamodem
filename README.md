# datamodem

A command-line softmodem. You give it SIP credentials and a phone number; it
places the call, negotiates a modem carrier in the audio band, and hands your
terminal to whatever is answering at the other end - a BBS, a dial-up host,
another modem. When you are done, the Hayes escape sequence - pause, `+++`,
pause - gets you back to a local `AT` prompt, and `ATH` hangs up.

```
$ datamodem 5551234 --server sip.example.com --username 1001
2026-09-29T20:13:12.114Z info  [sip] calling sip:5551234@sip.example.com
2026-09-29T20:13:16.402Z info  [sip] media active on call 0, modem attached
2026-09-29T20:13:21.630Z info  [modem] carrier established modulation=v34 rate=33600 role=originate
CONNECT 33600 V.42/V.42bis

Welcome to the thing on the end of the phone line.
Login:
```

It can answer as well as dial, and hand each incoming call to a program - a
BBS, a shell, `login` - the way getty hands a serial line to login.

There is no modem hardware and no sound card anywhere in this. The data pumps
are wired straight into a SIP media stream, so the modulated audio is the RTP
stream. It speaks:

- **V.34** up to 33600 bps, **V.32bis** up to 14400, **V.32** at 9600,
  **V.22bis** at 2400, **V.22** at 1200, **V.23** at 1200/75, **V.21** and
  **Bell 103** at 300 - stepping down automatically to whatever the far end
  is;
- **V.42** error correction and **V.42bis** compression on top of the
  modulations from V.22 up, falling back to plain async when the far end
  does not do them.

How it works, and everything learned building it, is in
[DEVNOTES.md](DEVNOTES.md).

## Installing

[Releases](https://github.com/billsimon/datamodem/releases) carry ready-built
binaries for macOS on Apple Silicon (11 or later), Linux on x86_64 and arm64
(glibc 2.34 or later: Debian 12, Ubuntu 22.04, RHEL 9 and anything since),
and Windows on x86_64. Each is one self-contained program; unpack it and run
it.

- **Linux**: it needs ALSA's `libasound.so.2`, which desktop distributions
  install anyway; on a server, `apt install libasound2` or the equivalent.
- **macOS**: the binary is not notarized, so if a browser downloaded it,
  `xattr -d com.apple.quarantine datamodem` before the first run.
- **Windows**: it is built with Cygwin, and `cygwin1.dll` has to stay in the
  same folder as `datamodem.exe`. Run it from Windows Terminal or any console.

## Building

```
brew install spandsp pjproject libtiff cmake pkg-config
cmake -S . -B build && cmake --build build
```

`libtiff` is only there because `spandsp.h` includes `<tiffio.h>` for the fax
half of the library; datamodem does not use it.

The build also compiles patched copies of spandsp's V.22bis and V.42 from
`third_party/`. The V.22bis in most packaged libspandsp builds trains and then
carries nothing, and the packaged V.42 cannot establish a link with a real
modem; [DEVNOTES.md](DEVNOTES.md) says why. `-DDATAMODEM_VENDOR_V22BIS=OFF`
and `-DDATAMODEM_VENDOR_V42=OFF` use the system library's instead.

A release build links spandsp, pjproject and OpenSSL statically, from
archives that `scripts/build-deps.sh` builds from pinned sources:

```
scripts/build-deps.sh deps
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DDATAMODEM_STATIC_DEPS=ON -DCMAKE_PREFIX_PATH=$PWD/deps
cmake --build build
```

That is what `.github/workflows/release.yml` does on macOS, Linux and
Cygwin for every push and pull request. Pushing a tag that matches
`include/datamodem/version.h`, such as `v0.1.0`, publishes the four builds
as a GitHub release.

## Using it

```
datamodem <number> [options]       dial it and hand over the terminal
datamodem dial <number> [options]  the same thing, spelled out
datamodem answer [options]         answer one inbound call and do the same
datamodem selftest [options]       loop two modems back to back, no SIP
datamodem version
```

```
export DATAMODEM_PASSWORD=...
datamodem +15551234567 --server sip.example.com --username 1001

# the defaults: V.34 up to 33600 bps, stepping down to whatever the far end
# is, with V.42 and V.42bis if the far end does them and plain async if not
datamodem 5551234 --server sip.example.com --username 1001

# start at V.32bis, 14400 bps (a far end that only does V.32 gets 9600)
datamodem 5551234 --server sip.example.com --username 1001 --modulation v32bis

# a host that wants 7E1 and speaks Bell 103
datamodem 5551234 --server sip.example.com --username 1001 \
    --modulation bell103 --data-bits 7 --parity even

# unattended: pipe a command in, collect the reply, hang up
echo -e 'help\r' | datamodem 5551234 --server sip.example.com --username 1001

# answer calls for a BBS, forever
datamodem answer --calls 0 --exec 'x84-modem-bridge --port 6510'
```

### Configuration

Every flag can also come from the environment as `DATAMODEM_<FLAG_IN_CAPS>`,
or from a `--config` file as `key = value`. The command line wins, then the
environment, then the file. In the file, `#` starts a comment - at the start
of a line, or after a space or tab once there is a value, so `escape-char = #`
and a password containing `#` mean what they say. Switches turn off with
`--no-`, as in `--no-tui`.

`datamodem help` lists every option, grouped by what it is for, and
[`examples/datamodem.conf`](examples/datamodem.conf) is a commented starting
point. Keep the password out of the file and in `DATAMODEM_PASSWORD`.

### The screen

Run from a terminal, datamodem works the way a 1990s terminal program did. It
takes over the screen, with a status line on the bottom row that follows the
call from start to finish:

```
 REGISTERING 00:00:01 | sip:1001@sip.example.com                CP437 | ctrl-c quits
 RINGING     00:00:04 | 180 Ringing                             CP437 | ctrl-c quits
 TRAINING    00:00:09 | v32bis training on the answerer         CP437 | ctrl-c quits
 ONLINE      00:12:31 | 14400 V.42/V.42bis | RX 48.2K TX 1.1K  CP437 | +++ for commands
 OFFLINE     00:12:40 | 14400 V.42/V.42bis | the far end cleared the call  CP437
```

Everything above it is the far end's screen. When the call ends the status
line stays where it is, frozen at the call's length, and the shell carries on
from a new line below it.

**Nothing the far end sends can reach your terminal directly.** Its bytes
drive an emulated ANSI-BBS screen - colour, cursor movement, erasing,
scrolling regions, the way ANSI.SYS understood them - and only what that
emulator draws is written to the real terminal. Line noise or a binary file
cannot change your terminal's modes, title or character set, or overwrite the
status line. It answers a BBS that asks where the cursor is, which is how
most of them decide whether the terminal does ANSI.

**Eight-bit characters are the far end's code page.** `--charset cp437`, the
default, is what every PC BBS drew its boxes and shading with; `--charset
utf8` is for a host that sends UTF-8, and `--charset ascii` shows anything
above 127 as `?`.

While a call is being set up, typing does not land on the screen and ctrl-c
still quits, including while it rings. Warnings appear on the status line for
a few seconds, errors are printed below it when the program ends, and the
full log goes wherever `--log-file` says. `--no-tui` turns the full screen off
for plain line-by-line output, which is still filtered to text and colour. It
is off anyway when stdin or stdout is not a terminal.

`--speaker` plays the call through the computer's sound device from the
moment the far end starts ringing until the modems have trained, the way a
modem's speaker does at its usual setting (ATM1): the ringing, the answer
tone, the handshake. It is off by default, and never opens the microphone.

### stdout is the line, stderr is the diagnostics

When stdout is not a terminal, it carries exactly the bytes that came off the
line, untouched, and everything datamodem has to say about itself goes to
stderr:

```
datamodem 5551234 > session.txt        # a clean transcript of the far end
datamodem 5551234 --log-file dm.log    # a clean terminal
```

The modem result codes - `CONNECT`, `NO CARRIER`, `OK` - and the `AT`
conversation are local, not remote, so they follow you: stdout when stdout is
your terminal, stderr when it has been redirected somewhere.

When stdin is a pipe, datamodem waits up to 30 s after it ends for a reply
before hanging up, which suits a script that sends a command and wants the
answer. `--hangup-on-eof` clears the call as soon as everything is sent.

### The escape sequence

The terminal goes into raw mode for the duration of the call. Every keystroke
reaches the far end unaltered, ctrl-C included - which is the point, and
which means `+++` is your way out:

1. at least one second of not typing (`--escape-guard-ms`, Hayes S12);
2. `+++` (`--escape-char`, Hayes S2), each within a second of the last;
3. at least one second of not typing again.

The guard time either side is what stops a `+++` inside real data from
dropping you into command mode. The plus signs are held back while the
sequence is in progress and sent on if it turns out not to be one, so nothing
is lost either way.

At the `OK` prompt:

| | |
|---|---|
| `ATO` | back to the call |
| `ATH`, `ATZ` | hang up |
| `ATI` | rate, protocol, duration, byte counts, compression ratio |
| `AT&V` | current settings |
| `ATE0` / `ATE1` | local echo off / on |
| `ATS2=n`, `ATS12=n` | escape character, guard time in ms |
| `AT?` | the above |

Received data queues up while you are at the prompt and arrives when you
`ATO`.

If the far end wedges while a large paste is still draining, `+++` cannot help
you: it needs a second of silence, and the queue keeps feeding the line.
`--escape-key '^]'` adds a single keystroke that escapes immediately. It is
off by default, because a real modem has no such thing.

### Answering for a BBS, or any other program

`--exec` hands each call to a program. Once the link is up, the command (run
by `/bin/sh -c`) gets the line as its stdin and stdout: what the caller sends,
it reads; what it writes, the caller receives. When it exits, the call is
cleared as soon as everything it said is on the wire. When the caller hangs up
first, its stdin ends and its process group is sent `SIGHUP`, as a dropped
carrier would, and `SIGKILL` five seconds later if it is still there.

```
datamodem answer --calls 0 --exec 'x84-modem-bridge --port 6510'
```

The line is a pipe, byte for byte, which is what a bridge like that wants -
and not a terminal, so a shell or `login` run this way has nothing to say:
`bash` reading a pipe prints no prompt, and `login` refuses to run at all.
`--exec-tty` gives the command a terminal instead, as getty would: a
pseudo-terminal set up the way a freshly opened serial line is - echoing,
line at a time, the caller's CR read as a newline and every newline sent as
CR LF, `^C` an interrupt. It is 80 by 24, its speed is the connect rate, and
when the caller hangs up it hangs up too.

```
datamodem answer --calls 0 --exec-tty --exec 'exec login'
datamodem answer --exec-tty --exec 'TERM=ansi exec bash -l'
```

`TERM` is whatever datamodem's own environment has, which is yours and not
the caller's; set it in the command, as above, for anything full-screen. A
program that wants raw bytes on a terminal puts it into raw mode itself, as
`rz`, `sz` and BBS software do.

The command's environment describes the call, under the names mgetty used, so
scripts written for it read them unchanged:

| | |
|---|---|
| `CONNECT` | the result code, `14400` or `14400 V.42/V.42bis` |
| `MODEM_RATE` | the bit rate, `14400` |
| `MODEM_MODULATION` | `v32bis`, as `--modulation` names it |
| `MODEM_PROTOCOL` | `async`, `V.42` or `V.42/V.42bis` |
| `MODEM_DIRECTION` | `answer`, or `originate` for `datamodem dial --exec` |
| `CALLER_ID` | the caller's number |
| `CALLER_NAME` | the caller's name, when the trunk sends one |
| `CALLED_ID` | the number that was called (dialled, for `originate`) |

Caller ID comes from `P-Asserted-Identity` when the trunk sends one, and
otherwise from `From`, which is whatever the caller says it is. datamodem's
own `DATAMODEM_*` settings - the SIP password among them - are not passed on.
The escape sequence is off with `--exec`: there is nobody to escape, and a
`+++` the program sends belongs on the line. Flow control works as it does
for a terminal, and V.42 passes it back to the caller.

`--calls` is how many calls `answer` takes before it exits: 1 by default, `0`
for as many as come. All of them share one registration. One call is answered
at a time; a call that arrives during another gets `486 Busy Here`. More lines
means more datamodems, each with its own SIP account, `--local-port` and RTP
range.

## Modulations

| `--modulation` | Standard | Rate | Calling end transmits | Answering end transmits |
|---|---|---|---|---|
| `v34` (default) | ITU-T V.34 | 33600 down to 2400, in steps of 2400, each direction its own | 2400 to 3429 symbols/s, carrier chosen by probing | the same band |
| `v32bis` | ITU-T V.32bis | 14400, 12000, 9600, 7200 or 4800 | 1800 Hz carrier | 1800 Hz carrier, the same band |
| `v32` | ITU-T V.32 | 9600 or 4800 | 1800 Hz carrier | 1800 Hz carrier, the same band |
| `v22bis` | ITU-T V.22bis | 2400 or 1200 | 1200 Hz carrier | 2400 Hz carrier |
| `v22` | ITU-T V.22 | 1200 | 1200 Hz carrier | 2400 Hz carrier |
| `v23` | ITU-T V.23 | 1200 down, 75 up | 390/450 Hz | 1300/2100 Hz |
| `v21` | ITU-T V.21 | 300 full duplex | 980/1180 Hz | 1650/1850 Hz |
| `bell103` | Bell 103 | 300 full duplex | 1270/1070 Hz | 2225/2025 Hz |

FSK frequencies are mark/space. V.34, V.32bis and V.32 have been used against
real modems - a Cisco MICA and BBSes - as well as against themselves.

The faster modulations train for longer and are far less tolerant of a poor
audio path: V.21 trains in about 4 seconds, V.22bis in about 6, V.32 in six
to ten depending on the round trip. V.34, V.32bis and V.32 pick their own
rate from the line they find, and V.34 and V.32bis change rate in the middle
of a call as the line does. If a trunk is doing anything at all
to the audio, the 300 bps modes will survive it and the faster ones will not -
and thirty characters a second is a perfectly usable interactive terminal.

### Stepping down

A modem with its factory settings connects to whatever answers - or calls -
at the fastest modulation the two have in common, and so does datamodem:

    v34 → v32bis → v32 → v22bis → v22 → v21 → bell103

Calling, it listens to what the answering modem sends and answers in kind.
Answering, it offers V.34, and to a caller that does not take it offers
V.22bis, V.32, V.21 and Bell 103 in turn until one replies.

- `--modulation` sets the fastest to try, and stepping down carries on from
  there. V.32bis also talks to a V.32 modem (at 9600), and V.22bis to a V.22
  one (at 1200).
- `--no-step-down` runs that one modulation and nothing else.
- `v23` is on no ladder and never steps down.
- `--bit-rate` caps the rate of whatever ends up running; a cap below anything
  the starting modulation can do starts lower instead, so `--bit-rate 1200`
  on its own is V.22bis at 1200.

Stepping down follows the far end's capabilities; it does not notice a bad
line. A path so poor that nothing faster than 300 bps will cross it needs
`--modulation v21` (or `bell103`) by hand.

## Error correction and compression

V.42 (LAPM) error correction and V.42bis compression run on top of every
modulation from V.22 up, and both are **on by default**, calling and answering, as on a
modem with its factory settings (`&Q5`, `%C1`). Each falls back gracefully,
and `CONNECT` says what you got:

```
CONNECT 14400 V.42/V.42bis   # both ends do V.42 and V.42bis
CONNECT 14400 V.42           # the far end will not compress
CONNECT 14400                # the far end does not do V.42: a direct async link
```

```
datamodem 5551234                          # all of the above
datamodem 5551234 --no-v42bis              # error correction, no compression
datamodem 5551234 --v42 off                # direct async only, no handshake
datamodem 5551234 --v42 require            # V.42 or nothing
```

| `--v42` | |
|---|---|
| `detect` (default) | run the V.42 handshake; fall back to direct async if the far end does not answer |
| `require` | run the handshake; give up on the call if the far end does not answer |
| `off` | direct async: start and stop bits, no error correction |

With V.42 running there are no start and stop bits on the line - the bit
stream is HDLC frames, retransmitted until they arrive intact - so
`--data-bits`, `--parity` and `--stop-bits` stop meaning anything.

V.42bis is an offer: `--v42bis-dict` and `--v42bis-max-string` (default 2048
and 32, what real modems shipped with) are the most it asks for, and the far
end may take less or none. It only ever runs inside V.42; compression on an
uncorrected link would turn one bad byte into garbage for the rest of the
call.

V.21, Bell 103 and V.23 never run V.42, whether a call starts on one or steps
down to it. At 300 bps the handshake alone would put several seconds of DC1
characters on the line, which a host without V.42 - and most at that speed
are - takes as typed input; V.23's 75 bps back channel cannot carry LAPM at
all. `--v42 require` is refused for those modulations, and a call that steps
down to one with it is hung up.

`ATI` reports what compression bought, per direction:

```
protocol    V.42/V.42bis
sent        4096 bytes, 358 on the wire (11.44:1)
received    112 bytes, 118 on the wire (0.95:1)
```

Repetitive terminal output, which is most of what a remote system sends, does
well; short bursts expand slightly. The V.42 handshake adds a second or two to
the connect, and compression usually more than pays it back.

## What the call needs

The modem lives in the audio band, so everything between here and the far end
has to leave the audio alone.

- **G.711 only.** datamodem offers PCMU and PCMA (`--codec`) and nothing
  else. A compressed codec - G.729, Opus, GSM - is built around the human
  voice and destroys a modem signal completely. A trunk or SBC that
  transcodes will do the same.
- **No packet loss concealment, VAD, echo cancellation or enhancement.**
  datamodem switches all of them off on its side; nothing in the path should
  turn them back on.
- **A fixed jitter buffer** (`--jitter-buffer-ms`, default 150), which never
  drops or stretches a frame. Bigger costs round-trip delay an interactive
  session can feel; smaller starves the receiver. 60 is usually fine on a
  LAN.
- **The RTP port range.** Media stays inside `[--rtp-port, --rtp-port +
  --rtp-port-range]`. Open exactly that range on the firewall and the SBC; a
  narrower allowance shows up as a call that connects and then never trains.
- **NAT.** Behind one, set `--public-addr` or `--stun`.

## When it goes wrong

Run the call again with `--log-level debug --log-file call.log`. Every stage
of every handshake is logged - which tone was heard, the measured round trip,
what each end offered, every V.42 frame - so the log says where it stopped.
`DATAMODEM_RECORD=/tmp/call` also records the call's audio, to
`/tmp/call.<tag>.wav`: what was heard on the left, what was sent on the right.

- **Connects, then a screen of one repeated character** (often `U`).
  datamodem warns about this: it is a demodulator that never locked, not
  data. Check that nothing is transcoding the audio, and try a fixed
  `--modulation`.
- **Never trains.** Usually the audio path: a compressed codec, a transcoding
  trunk, or RTP ports blocked. `datamodem selftest` (below) rules out the
  modem itself.
- **Keeps retraining, then hangs up.** The line will not hold the rate.
  `--max-retrains` (default 4) ends such a call with a diagnosis; try a lower
  `--bit-rate`, or `--modulation v21`, which is far more tolerant of a poor
  audio path.
- **The host sees junk at the start of the call.** While V.42 is detecting,
  a calling modem sends DC1 characters, which an async host takes for XON,
  for a second or so (never at 300 bps, where V.42 does not run). Real modems did the same; `--v42 off` spares a host
  that chokes on it.
- **The carrier drops.** A carrier that disappears is not the end of the
  call: datamodem waits four seconds for it to come back (V.34 retrains for
  up to fifteen), and only then reports `NO CARRIER`.
- **The far end seems hung.** Some BBSes wait for a key before they say
  anything - Mystic's "press ESC twice" botcheck, for one.

## Testing

```
./build/datamodem selftest                    # two modems in memory, no SIP
./scripts/loopback-test.sh                    # a real call over real RTP on loopback
ctest --test-dir build                        # unit tests
```

`selftest` runs two modems back to back, pushes data each way and compares it
byte for byte. It needs no network and no credentials, and it is the fastest
way to tell a modem problem from a SIP problem:

```
for m in v34 v32bis v32 v22bis v22 v23 v21 bell103; do
    ./build/datamodem selftest --modulation $m --no-step-down
done
```

The full set of test scripts, and the environment hooks that put noise, echo,
delay and dropouts on the selftest's line, are described in
[DEVNOTES.md](DEVNOTES.md#testing).

## Exit codes

| | |
|---|---|
| 0 | the call ran and was cleared normally |
| 2 | bad flags |
| 3 | missing credentials or nonsense settings |
| 4 | SIP transport or registration failure |
| 5 | call rejected, busy, or never answered |
| 6 | answered, but the link never carried data - never trained, or `--v42 require` and the far end does not do V.42 |
| 7 | a deadline was hit |
| 8 | internal error |
