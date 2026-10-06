# datamodem

A command-line softmodem. You give it SIP credentials and a phone number; it
places the call, negotiates a modem carrier in the audio band, and hands your
terminal to whatever is answering at the other end. When you are done, the
Hayes escape sequence — pause, `+++`, pause — gets you back to a local `AT`
prompt, and `ATH` hangs up.

```
$ datamodem 5551234 --server sip.example.com --username 1001
2026-09-29T20:13:12.114Z info  [sip] calling sip:5551234@sip.example.com
2026-09-29T20:13:16.402Z info  [sip] media active on call 0, modem attached
2026-09-29T20:13:21.630Z info  [modem] carrier established rate=300 role=originate
CONNECT 300

Welcome to the thing on the end of the phone line.
Login:
```

There is no modem hardware and no sound card anywhere in this. The data pumps
- spandsp's, and a V.32, V.32bis and V.34 of our own - are wired straight into a pjsip media
port, so the modulated audio is the RTP stream.

Sibling project: [`faxmodem`](../faxmodem) does the same thing for T.30 fax.
datamodem borrows its structure — the option handling, the logging, the pjsua
setup and codec discipline are deliberately the same shape.

## Building

```
brew install spandsp pjproject libtiff cmake pkg-config
cmake -S . -B build && cmake --build build
```

`libtiff` is only there because `spandsp.h` includes `<tiffio.h>` for the fax
half of the library; datamodem does not use it.

The build also compiles `third_party/spandsp-v22bis`, which is spandsp's own
V.22bis from a later snapshot than most packages ship. Without it V.22bis
connects and then carries nothing — see below. `-DDATAMODEM_VENDOR_V22BIS=OFF`
uses the system library's instead.

## Using it

```
datamodem <number> [options]       dial it and hand over the terminal
datamodem dial <number> [options]  the same thing, spelled out
datamodem answer [options]         answer one inbound call and do the same
datamodem selftest [options]       loop two modems back to back, no SIP
datamodem version
```

Every flag can also come from the environment as `DATAMODEM_<FLAG_IN_CAPS>`,
or from a `--config` file as `key = value`, where `#` starts a comment -
at the start of a line, or after a space or tab once there is a value, so
`escape-char = #` and a password containing `#` mean what they say. Command
line wins, then the environment, then the file. `datamodem help` lists all
of them; `examples/datamodem.conf` is a commented starting point.

```
export DATAMODEM_PASSWORD=...
datamodem +15551234567 --server sip.example.com --username 1001

# the defaults: V.34 up to 33600 bps, stepping down to whatever the far end
# is, with V.42 and V.42bis if the far end does them and plain async if not
datamodem 5551234 --server sip.example.com --username 1001

# 14400 bps, where the far end is a V.32bis (or V.34, V.90...) modem; a far
# end that only does V.32 gets 9600
datamodem 5551234 --server sip.example.com --username 1001 --modulation v32bis

# 9600 bps, plain V.32
datamodem 5551234 --server sip.example.com --username 1001 --modulation v32

# a host that wants 7E1 and speaks Bell 103
datamodem 5551234 --server sip.example.com --username 1001 \
    --modulation bell103 --data-bits 7 --parity even

# unattended: pipe a command in, collect the reply, hang up
echo -e 'help\r' | datamodem 5551234 --server sip.example.com --username 1001
```

### The screen

Run from a terminal, datamodem works the way a 1990s terminal program did.
It clears the screen and takes it over, with a status line on the bottom
row that follows the call from start to finish:

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

**Nothing the far end sends can reach the terminal itself.** A BBS sends ANSI
- colour, cursor movement, screen clears - and line noise or a binary file
sends anything at all, some of which, passed to a terminal, would switch its
character set, change its modes or its title, move its scroll region or
overwrite the status line, and some of that outlives the program. So the far
end's bytes drive an emulated screen instead: ANSI-BBS, the way ANSI.SYS and
the BBS terminals understood it, with colour, cursor movement, erasing,
inserting and deleting, scrolling regions and saved cursors. Only what that
emulator generates itself is ever written to the real terminal. It answers a
BBS that asks where the cursor is - which is how most of them decide whether
the terminal does ANSI - for the screen it is actually drawing on.

**Eight-bit characters are the far end's code page.** `--charset cp437`, the
default, is what every PC BBS drew its boxes and shading with, translated to
the Unicode a modern terminal shows; `--charset utf8` is for a host that
sends UTF-8, and `--charset ascii` shows anything above 127 as `?`.

While a call is being set up, typing does not land on the screen and ctrl-c
still quits - including while it rings. Warnings appear on the status line
for a few seconds rather than in the middle of the far end's screen, errors
are printed below it when the program ends, and the full log goes wherever
`--log-file` says. `--no-tui` turns all this off for plain line-by-line
output - which is still made safe to print, letting through text, colour and
nothing else, and still answers a BBS that asks where the cursor is. It is
off anyway when stdin or stdout is not a terminal.

### stdout is the line, stderr is the diagnostics

When stdout is not a terminal, it carries exactly the bytes that came off
the line, untouched, and everything datamodem has to say about itself goes
to stderr. So:

```
datamodem 5551234 > session.txt        # a clean transcript of the far end
datamodem 5551234 --log-file dm.log    # a clean terminal
```

The modem result codes — `CONNECT`, `NO CARRIER`, `OK` — and the `AT`
conversation are local, not remote, so they follow you: stdout when stdout is
your terminal, stderr when it has been redirected somewhere.

A slow log cannot hurt the call. Lines from the media and SIP threads are
queued and written by a thread of their own, so a log that stops accepting
writes - a terminal over a bad link, a pipe nobody is reading - costs log
lines (counted, and said so when it recovers) rather than audio. Before
this, a debug log into a full pipe stalled the modem mid-frame and dropped
the call.

### The escape sequence

The terminal goes into raw mode for the duration of the call, with `ISIG` and
`OPOST` off. Every keystroke reaches the far end unaltered, ctrl-C included —
which is the point, and which means `+++` is your way out:

1. at least one second of not typing (`--escape-guard-ms`, Hayes S12);
2. `+++` (`--escape-char`, Hayes S2), each within a second of the last;
3. at least one second of not typing again.

The guard time either side is what stops a `+++` inside real data from
dropping you into command mode. The plus signs are held back while the
sequence is in progress and released onto the line if it turns out not to be
one, so nothing is lost either way.

At the `OK` prompt:

| | |
|---|---|
| `ATO` | back to the call |
| `ATH`, `ATZ` | hang up |
| `ATI` | rate, duration, byte counts |
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

`--exec` hands each call to a program, the way getty hands a serial line to
login. Once the link is up, the command (run by `/bin/sh -c`) gets the line
as its stdin and stdout: what the caller sends, it reads; what it writes, the
caller receives. When it exits, the call is cleared as soon as everything it
said is on the wire. When the caller hangs up first, its stdin ends and it is
sent `SIGHUP` - to its whole process group, as a dropped carrier would - and
`SIGKILL` five seconds later if it is still there.

```
datamodem answer --calls 0 --exec 'x84-modem-bridge --port 6510'
```

The command's environment says what is known about the call, under the
names mgetty gave them, so scripts written for it read them unchanged:

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
own `DATAMODEM_*` settings - the SIP password among them - are not passed
on. The program sees nothing of `+++`: the escape sequence is off with
`--exec`, since there is nobody to escape and a `+++` the program sends
belongs on the line. Flow control works the same way it does for a
terminal: datamodem reads the program's output only as fast as the line
takes it, and stops taking the caller's bytes from the modem when the
program falls behind, which V.42 then passes back to the caller.

`--calls` is how many calls `answer` takes before it exits: 1 by default,
`0` for as many as come. All of them share the one registration, so there
is no gap between calls while it registers again, as there would be
running datamodem once per call. One call is answered at a time; a call
that arrives during another gets `486 Busy Here`. More lines means more
datamodems, each with its own SIP account, `--local-port` and RTP range.

`--hangup-on-eof` is the same prompt clearing for a plain pipe: by default,
when stdin ends datamodem waits up to 30 s for a reply before it hangs up,
which suits a script that sends a command and wants the answer; with it,
the call is cleared as soon as everything is sent.

## What it can actually do
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

All eight carry data, verified byte-for-byte in both directions by `selftest`
and over a real call by `loopback-test.sh`. **V.22bis needs the sources in
`third_party/spandsp-v22bis` built in** — the V.22bis in most packaged
libspandsp builds does not work; see below.

Frequencies are mark/space. V.21 and Bell 103 number their channels in
opposite directions — V.21 channel 1 is the calling modem's, Bell 103's is
the answering modem's — and spandsp follows each convention faithfully, which
is an easy way to end up with both modems transmitting into each other's
band. Nothing in the test suite can catch that, because both ends of every
test are this same program and invert together; the table above is checked
against the standards rather than against a passing test.

2400 bps is thirty times faster than 300 and makes an interactive session
genuinely comfortable; 9600 makes a file transfer reasonable, and 14400 half
as long again. Against that,
V.22bis takes about 6 seconds to train where V.21 takes about 4 - V.32 six
to ten, depending on the round trip - and QAM is far less tolerant of a poor
audio path than FSK is. If a trunk is doing anything at all to the audio,
the 300 bps modes will survive it and the QAM ones will not.

The default is `v34`, stepping down to whatever the far end turns out to be
— see below. What stepping down does not do is notice a bad line: it follows
the far end's capabilities, and V.34 copes with a poor line by choosing a
lower rate within V.34. A path so bad that nothing faster than 300 bps will
cross it needs `--modulation v21` (or `bell103`) by hand, and thirty
characters a second is a perfectly usable interactive terminal — it is how
everyone did this in 1982.

### Stepping down

A modem with its factory settings connects to whatever answers — or calls —
at the fastest modulation the two have in common, and so does datamodem:

    v34 → v32bis → v32 → v22bis → v22 → v21 → bell103

V.32bis already trains with a far end that is only V.32 (at 9600), and
V.22bis with one that is only V.22 (at 1200), so `v32` and `v22` are only
ever where a `--modulation` starts. `--modulation` sets the fastest to try,
and stepping down carries on from there; `--no-step-down` runs that one
modulation and nothing else. `v23` is on no ladder and never steps down.
`--bit-rate` still caps the rate of whatever ends up running, and one below
anything the starting modulation can do starts lower instead: `--bit-rate
1200` on its own is V.22bis at 1200.

**Calling**, datamodem listens for what the answering modem sends before it
has heard anything it recognises, and answers in kind:

| It hears | Which means | So it runs |
|---|---|---|
| ANSam (2100 Hz, amplitude modulated) | V.8 | V.34 |
| a plain answer tone | no V.8: an older modem | V.32bis — AA once it has heard a second of the tone |
| AC (600 + 3000 Hz) | V.32 | V.32bis |
| USB1, V.22's unscrambled ones | V.22bis or V.22 | V.22bis, once it has sent AA long enough for a V.32 answerer to have heard it, or else after 3.1 s more of USB1 (V.32bis Annex A's Tc, in case AC follows) |
| 1650 Hz | V.21 channel 2 | V.21 |
| 2225 Hz | Bell 103's answering carrier | Bell 103 |

**Answering**, it has to offer, because a V.22bis, V.21 or Bell 103 caller
says nothing until it hears its own kind of answering signal. With V.34 at
the top it sends ANSam and listens for V.8's CM; a caller that does not
answer with one gets each of these for three seconds in turn, round and
round until `--train-timeout`, and the first to be answered is kept:

1. V.22bis's USB1, listening for S1 or SB1 (Annex A's Ta);
2. V.32's AC, listening for AA (three seconds plus the round trip);
3. V.21's channel 2 carrier, listening for channel 1;
4. Bell 103's 2225 Hz, listening for 1270 Hz.

A caller that sent V.32's AA during the answer tone — which an automode V.32
caller does — goes straight to V.32bis, and so does one heard sending AA at
any later point. At 300 bps it is the same: a V.21 or Bell 103 caller heard
during any offer is answered in its own modulation at once.

Telling these apart is done with Goertzel filters over 40 ms blocks, against
the block's whole power less the echo of whatever this end is sending. The
hard pair is Bell 103's 2225 Hz and USB1, which puts most of its power at
2250 Hz: 40 ms makes those two bins orthogonal, and USB1's second line,
600 Hz higher and a fourteenth of its power, settles it. V.21 and Bell 103
carrying data — V.42's ODP, say — smear between their tones, and their
calling bands interleave 90 Hz apart, so which one it is is decided over the
whole run of blocks rather than one at a time.

V.21 and Bell 103 now wait to hear the far end's tone before they connect, at
either end, rather than taking any energy for a carrier — a V.32 caller's AA,
or the far end's echo of our own, used to be enough. A calling FSK modem
stays silent until it hears the answering carrier, as the real ones did, and
then holds its own at mark for half a second plus the path before it passes
data, so the answerer is listening when the first character arrives.

### The V.22bis situation, and why `third_party` exists

The V.22bis in the libspandsp that Homebrew (and several distributions) ship
**does not carry data**. It completes the full training exchange, reports a
connection, and then delivers a constant `0x55` for the rest of the call. The
cause is measurable: its receive equaliser diverges to NaN about a second
*before* training claims success, so every symbol afterwards slices to the
same constellation point and the differential decoder emits one fixed dibit
forever. Training "succeeds" because nothing checks that the equaliser
converged.

This is not a bug in V.22bis as written. Homebrew builds the `spandsp-0.0.6`
release tarball from 2012. A later upstream snapshot — still numbered 0.0.6,
but with sources dated into mid-2014 — has a V.22bis that works. Same API,
same call sequence, no patching: the difference is entirely which vintage of
the code you compile.

So `third_party/spandsp-v22bis/` holds that snapshot's two V.22bis source
files and their generated filter tables. They are compiled into datamodem and
override the library's versions; everything else still comes from the shared
library. Nothing else in libspandsp touches a `v22bis_state_t`, and every
header involved is byte-identical between the two vintages, so the override
is ABI-safe. That directory's README has the details, including how to
regenerate the filter tables.

Turn it off with `-DDATAMODEM_VENDOR_V22BIS=OFF` if your libspandsp is new
enough; datamodem will warn at runtime that it is trusting the system one,
and `datamodem selftest --modulation v22bis` settles whether that trust is
justified.

This is worth knowing if you use any other spandsp-based softmodem. The
Asterisk `app_softmodem` family makes the identical three calls, and its
V.22bis has carried a "still untested" note since the 2010 commit that added
it; its issue trackers have long-standing reports of exactly this symptom —
a short fixed repeating byte pattern, forever.

### V.32: 9600 bps, full duplex

`--modulation v32` is ITU-T V.32 (03/93), written for this project in
`src/v32.c` - spandsp has no V.32, and nothing else free does either. It
offers 9600 bps trellis coded, the 16-point nonredundant 9600 that every
9600 bps V.32 modem must be able to fall back to, and 4800, and the two ends
agree which in the rate exchange of section 5. `--bit-rate 4800` offers only
4800. 2400, which the Recommendation leaves "for further study", is not
there.

```
datamodem 5551234 --server sip.example.com --username 1001 --modulation v32
...
info  [v32] trained at 9600 (trellis coded): SNR 38.9 dB, round trip 300 ms,
      echo canceller off - no echo (tag=out)
CONNECT 9600
```

Four times V.22bis, and still full duplex, which is what a terminal needs.
It works because of the thing that makes V.32 different from the fax modems
that share its constellation: both directions use the *same* band, a 2400
baud carrier at 1800 Hz, and each modem separates the other's signal from
its own by subtracting its own echo.

**Why the echo canceller matters over SIP.** Into the telephone network, the
far end's line card has a 2-wire hybrid, and it reflects our own signal
straight back at us a whole round trip later - two jitter buffers and the
network, so typically 200-600 ms. The phase-reversed answer tone V.32 uses
(datamodem sends it when answering, as V.25 says) tells the network's own
echo cancellers to stand aside, because V.32 expects to do the job itself.
An echo only 10 dB down makes 9600 impossible, and they are often worse. So:

- The start-up procedure measures the round trip, from the phase reversals
  in the AA/AC/CA/CC tone exchange, exactly as section 5.4 lays out - which
  also tells it how far back to look for the echo.
- During its own training sequence, with the far end silent, each modem
  cross-correlates what comes back against what it sent, finds the echo,
  and puts a 16 ms adaptive canceller around it. Its TRN is stretched (the
  Recommendation allows up to 8192 symbols) by the round trip, so that the
  echo of its start has time to come back and be learned before the far end
  starts talking.
- Once the far end is talking the canceller keeps adapting, slowly, so it
  follows the small drift of a real path - quickly for the first few
  seconds, then at a step small enough that the far end's own signal, which
  is noise as far as the canceller is concerned, does not leak into it. At
  the old fixed step that leak was 30 dB down: no matter at 9600, a third of
  the margin at 14400.

Two datamodems talking pure VoIP have no hybrid anywhere and no echo; the
log says `echo canceller off - no echo`, which is correct, not a fault.

**What it does with a poor line.** Each end measures how well its receiver
trained and only offers 9600 if the margin is there, so a noisy path settles
at 4800 on its own rather than connecting fast and corrupting. A link whose
reception degrades in the middle of a call retrains (5.5) - the whole
start-up again, which is around four seconds on a 150 ms path - and picks a
rate again; `--max-retrains` still ends one that will not hold. Measured with
`selftest` over G.711 with a 150 ms path each way and the far end's echo
coming back 10 dB down:

| noise | rate agreed | receiver SNR | result |
|---|---|---|---|
| none | 9600 trellis | 32-35 dB | byte-for-byte |
| -40 dBm0 | 9600 trellis | 27-28 dB | byte-for-byte |
| -31 dBm0 | 9600 trellis | 19.5-20 dB | byte-for-byte |
| -29 dBm0 | 4800 | 18 dB | byte-for-byte |
| -24 dBm0 | 4800 | 13 dB | byte-for-byte |

The trellis code is worth about 3 dB over the uncoded 16-point
constellation. The thresholds come from a minute of pseudo-random data each
way at every rate and noise level, measured bit by bit: each coding is
offered a dB above the SNR where it stopped making errors at all (19 dB for
9600 trellis coded), and a running link counts as poor where it reaches
about one error in ten thousand (17 dB). The receiver's SNR estimate tracks
what training predicted to within 0.3 dB, which is what makes choosing a
rate from training work. Longer runs in a separate harness ran two minutes without a bit error
with the echo 3 dB down and a 250 ms path, with the far modem's clock 1000
ppm off ours (ten times what V.32 permits), and through a dispersive
channel; 20-60 ms of lost audio costs the bits in it and no retrain.

**What it has not met yet is another manufacturer's V.32.** Everything above
is datamodem against datamodem, which has exactly the weakness the V.42
sections below describe: two copies of the same code agree with each other
even where they are both wrong. The parts most likely to matter against real
hardware have been checked against the Recommendation itself rather than
against a passing test - the constellation maps (Figures 1 and 3 and Table 3
agree, and the trellis code is transparent to 90-degree rotations, which it
would not be if a map were wrong), the scramblers (the TRN patterns in 5.2.3
come out exactly as printed), the rate sequences (Tables 6 and 7) and the
timing of the start-up exchange (the 64-symbol turn-rounds, which is why the
measured round trip comes out at precisely the two jitter buffers). The log
at `--log-level debug` narrates every step of the handshake - which tone was
heard, the measured round trip, the rates each side offered - so a failure
against a real modem says where. The Annex A automode fallback to V.22bis is
not implemented, so the far end has to be doing V.32 (or V.32bis, V.34 and
so on, which fall back to it).

A caller waits for the answer tone, as with every other modulation, and
starts its AA one second into it (5.4.1). If the answering modem skips the
answer tone and goes straight to AC, which V.32 allows on national
connections, the caller hears that and starts at once instead of waiting out
`--answer-wait`.

### V.32bis: 14400 bps

`--modulation v32bis` is ITU-T V.32bis (02/91), in the same `src/v32.c`: V.32
with three more trellis coded constellations - 14400 (128 points), 12000
(64) and 7200 (16) - alongside V.32's 9600 and 4800, and a way of changing
rate in the middle of a call without retraining.

```
datamodem 5551234 --server sip.example.com --username 1001 --modulation v32bis
...
info  [v32] trained at 14400 (trellis coded): SNR 33.5 dB, round trip 300 ms,
      echo canceller on (tag=out)
CONNECT 14400
```

Everything V.32 does above, V.32bis does the same way: the same start-up,
the same echo canceller, the same trellis code and the same 9600
constellation, point for point. What changes is the meaning of the rate
signals. A V.32bis modem marks its own with two bits V.32 left alone, and if
the far end's lack them the call runs as V.32 - so `--modulation v32bis`
dialling a V.32 modem connects at 9600, says `trained at 9600 ..., as V.32`,
and is exactly the V.32 above. Both BBSes this was tried against announce
themselves as V.32bis in their very first rate signal (`0ff9: V.32bis 4800
7200 9600 12000 14400`) and were answering `--modulation v32` as V.32.

`--bit-rate` caps what is offered: 14400, 12000, 9600, 7200 or 4800.

**Choosing the rate.** As with V.32, each end offers only what its receiver
trained well enough for, a dB above where each coding stopped making errors.
The same conditions as the V.32 table:

| noise | rate agreed | receiver SNR | result |
|---|---|---|---|
| none | 14400 | 33 dB | byte-for-byte |
| -40 dBm0 | 14400 | 27.5 dB | byte-for-byte |
| -36 dBm0 | 12000 | 24.5 dB | byte-for-byte |
| -33 dBm0 | 9600 | 22 dB | byte-for-byte |
| -29 dBm0 | 7200 | 18 dB | byte-for-byte |
| -24 dBm0 | 4800 | 13 dB | byte-for-byte |

Each further bit per symbol costs 3 dB: 14400 wants about 25 dB, which a
clean G.711 path gives with room to spare. 7200 needs no more than 4800
does, to within a dB, and is half as fast again.

**Changing rate without retraining** (section 8). Between two V.32bis modems
either end can ask for a different rate in the middle of data: a preamble,
an exchange of rate signals, and 24 symbols at the new rate, a fraction of a
second in all where a retrain is several. The equaliser, the loops and the
echo canceller carry on as they were. datamodem answers the far end's
requests, and makes its own:

- **down**, when reception has been poor for two seconds, to the best rate
  the receiver would bear now - which is what V.32 can only retrain for;
- **up**, when reception has been good enough for a higher rate, with a dB
  to spare, for ten seconds: after a step down that was more cautious than it
  needed to be, or a bad patch that passed. An attempt that gets nothing -
  the far end's receiver would not have it - waits twice as long before the
  next.

```
info  [v32] reception has been poor (18.3 dB SNR) for 2 seconds; asking the far end to change to 7200 (tag=out)
info  [v32] changed rate to 7200 (trellis coded) without retraining: SNR 17.8 dB (tag=out)
...
info  [v32] reception is good enough for 14400 (31.5 dB SNR); asking the far end to change to 14400 (tag=out)
info  [v32] changed rate to 14400 (trellis coded) without retraining: SNR 29.9 dB (tag=out)
```

The Recommendation sends the rate change in the middle of whatever was
crossing, so the bits in flight at that moment are lost: V.42 retransmits
them, and without V.42 a character or two goes astray, exactly as with a
retrain. A far end that does not answer a request - the Recommendation
warns that some V.32 modems use the marking bit for something else - or
starts the exchange and does not finish it, is retrained with instead, and
not asked again; its own requests are still answered.

Datamodem against datamodem it has run fifteen minutes at 14400 without a
bit error, with the far end's echo 10 dB down, a 150 ms path, G.711 and the
far modem's clock 50 ppm off ours; it holds 14400 from -100 to +200 ppm.
**What it has not met yet is a real V.32bis modem at 14400**: the
constellations are checked point by point against the Recommendation's
figures (and agree with spandsp's V.17, which shares them), and the rate
signals against its tables, but the same caution as for V.32 applies until
it has.

### V.34: 33600 bps

`--modulation v34` is ITU-T V.34 (02/98), in `src/v34.c` with the bit-exact
parts split out: `src/v34_codec.c` (framing, shell mapping, the trellis codes,
precoding, the Viterbi decoder), `src/v34_info.c` (INFO sequences, MP, V.8's
CM/JM/CJ) and `src/v34_dsp.c` (the Phase 2 DPSK, probing and its analysis).

```
datamodem 5551234 --server sip.example.com --username 1001 --modulation v34
```

The start-up is four phases, each logged as it goes:

1. **V.8.** The answerer sends ANSam, the 15 Hz-modulated answer tone; the
   two exchange CM, JM and CJ at 300 bit/s and agree on V.34 and, if both
   offer it, LAPM.
2. **Probing and ranging.** INFO0 each way at 600 bit/s, the round trip
   measured from reversals of tones A and B, then the 21-tone probe (L1,
   L2). Each end works out from what it heard which of the six symbol rates,
   which carrier and which pre-emphasis filter the line will carry, and how
   fast, and tells the other in INFO1. Logged as `probed: sending at ...`.
3. **Equaliser and echo canceller training** at the chosen symbol rate (S,
   PP, TRN, J).
4. **The MP exchange**, which settles the data rate in each direction from
   the SNR the receiver measured on TRN. Logged as `trained: ...`.

The two directions are independent: different symbol rates, carriers and
data rates are normal, and `CONNECT` reports the rate we receive at.
`--bit-rate` caps both. Both ends of a retrain (11.5), rate renegotiation
without one (11.6) and cleardown (11.7) are implemented; a carrier lost for
up to 15 seconds is retrained, not hung up on.

Our receiver asks for the 16-state trellis code, no precoding and no
non-linear encoder - which is what keeps it to a linear equaliser - but the
transmitter does everything the far end's MP can ask for: all three trellis
codes, precoding with the far end's coefficients, the non-linear encoder and
expanded shaping.

**A far end that does not do V.8** is taken to be an older modem, and the
call carries on as V.32bis (`falling back to V.32 bis at up to 14400 bps`):
as the caller, on hearing a plain answer tone rather than ANSam; as the
answerer, when ANSam gets no CM. A far end that does V.8 but offers no V.34
is hung up on.

**Not implemented:** half duplex (clause 12), the auxiliary channel and
Annex A.

**What it has been tested against.** Datamodem against itself only, so far:
over the selftest's simulated line at every symbol rate, both carriers, all
pre-emphasis filters and all three codes, through G.711, with echo down to
-6 dB, round trips to 700 ms, noise, bursts, dropouts, and clocks to
±100 ppm apart; and over a real SIP call on loopback, at 33600 with V.42.
Because both ends of every test are this code, the parts that have to be bit
exact with somebody else's modem - the superconstellation, the shell
mapper's tables, the trellis encoders, the framing of every rate - are
checked against the Recommendation's tables and against spandsp's
independent tables (`tests/v34_test.c`), not just against each other. **What
it has not met is a real V.34 modem.** The paths only a real modem will
exercise are V.8 interop, and the precoder, non-linear encoder and 32/64-state
codes on transmit, since our own receiver never asks for them. Run those
calls with `--log-level debug`: every phase logs what it heard and why it
decided what it did.

V.90 is a different machine again, and nothing here implements it.

## Error correction and compression

V.42 (LAPM) and V.42bis are both implemented, on top of any modulation, and
both are **on by default**, calling and answering, the way a modem with its
factory settings had them (`&Q5`, `%C1`). Each falls back gracefully:

- both ends do V.42 and V.42bis: `CONNECT ... V.42/V.42bis`;
- the far end does V.42 but will not compress, or takes less of the
  dictionary than offered: `CONNECT ... V.42`, or V.42bis on its terms;
- the far end does not do V.42 at all: a direct async link, `CONNECT ...`.

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

With V.42 running there are no start and stop bits on the line at all — the
bit stream is HDLC frames, retransmitted until they arrive intact. `--data-bits`,
`--parity` and `--stop-bits` stop meaning anything, because there is no
character framing left for them to describe. `CONNECT` says which you got:

```
CONNECT 300                  # direct async
CONNECT 300 V.42             # error corrected
CONNECT 300 V.42/V.42bis     # error corrected and compressed
```

`--v42bis` is an offer made inside V.42's XID exchange: the far end may take
less of it, or none - see below. With `--v42 off`, or after a fall back to
async, there is nothing to make it in and it does not run. That is as it
should be: compression on an uncorrected link is worse than none, because
both ends build a shared dictionary as they go, so a single corrupted byte
desynchronises them and everything after it is garbage rather than one bad
character. V.23 never runs V.42 (its 75 bps back channel cannot carry LAPM):
`detect` runs it without, and `require` is refused.

`ATI` reports what compression actually bought, per direction:

```
protocol    V.42/V.42bis
sent        4096 bytes, 358 on the wire (11.44:1)
received    112 bytes, 118 on the wire (0.95:1)
```

Short bursts expand slightly — there is nothing in six characters for a
dictionary to exploit — and V.42bis falls back to transparent mode rather
than making things much worse. Repetitive terminal output, which is most of
what a remote system sends, does well: a menu redrawn twice is nearly free.

### The timer bug, and why V.42 used to fail against real modems

spandsp measures every V.42 timer in bit periods — the detection timeout
T400, the retransmission timer T401, the idle poll T403 — and converts from
milliseconds using a transmit rate it **hardcodes to 28800 bps** at init,
with no API to change it. At 2400 bps that makes every timer twelve times
too long; at 300 bps, ninety-six times.

Two spandsp instances inflate by the same factor and interoperate perfectly,
which is exactly why this hides until you meet real equipment. A real modem's
T401 fires on schedule, it retransmits into a peer that will not answer for
another ten or ninety seconds, it runs out of retries, and the link never
comes up. Measured, as the lag between the last byte being delivered and the
sender's acknowledgement bookkeeping catching up:

| line rate | as shipped | corrected | inflation |
|---|---|---|---|
| 300 bps | 96.30s | 1.27s | 96× |
| 600 bps | 48.15s | 1.73s | 48× |
| 1200 bps | 24.07s | 1.07s | 24× |
| 2400 bps | 12.04s | 1.04s | 12× |

`28800/rate` in every case, and the corrected figure is T401 = 1000 ms, as
specified (longer only on a path measured to need it - see "V.42bis is
negotiated" below). datamodem sets the rate before `v42_restart()` — after it is too
late, because the restart has already armed T400 from the stale value.

Two consequences worth knowing:

- **Detection failure is now reported.** With the timers inflated, T400 took
  so long to expire that spandsp appeared never to say anything; corrected,
  it reports `LAPM_V42_UNSUPPORTED` on schedule and datamodem falls back to
  direct async immediately instead of waiting out `--v42-timeout`.
- **T400 gets a floor of 1024 bit periods.** Detection recognises a repeating
  pattern, so what it needs is bits, not milliseconds — about 512 of them.
  The spec's 750 ms supplies that at 1200 bps and up but not at 300. Floored,
  because stretching T400 only delays our own giving-up and never makes the
  far end wait; the interop-critical timers stay spec-correct.

### When V.22bis settles at 1200, check both ends agree

V.22bis says "I can do 2400" by sending the S1 pattern during training. An
end that misses it settles at 1200. If the far end did send S1 and stayed at
2400, the two are now demodulating each other at different rates: both see
noise, the far end keeps asking to retrain, and eventually somebody clears
the call. The training log tells the two apart:

```
S1 detected (61 long)                 <- saw it; goes to 2400
starting 16 way decisions (caller)
Rx normal operation (2400)
```
```
starting S11 after U0011              <- no S1; settles at 1200
Rx normal operation (1200)
```

The symptom is a call that connects at 1200, delivers nothing but
high-entropy garbage, and retrains every few seconds. `--max-retrains`
(default 4) now ends such a call with that diagnosis rather than letting it
run:

```
error [session] the link retrained 4 times and will not hold. It settled at
1200 bps having offered 2400, which usually means the two ends disagreed
about the rate - try --bit-rate 1200 so both start there, or --modulation
v21, which is far more tolerant of a poor audio path.
```

Three separate bugs made this happen, and all three are now fixed; a call
that negotiates down to 1200 holds on its own, and `--bit-rate 1200` is no
longer needed.

**The two ends could commit to different rates.** V.22bis settles the 2400
question once, during training, with the S1 pattern: the calling modem sends
it and the answering modem echoes it back if it agrees. Each decision is made
from what that end heard, inside a window that closes for good when training
ends. On a phone line that is safe. Over RTP it is not — lose the 100 ms
carrying the far end's reply and we conclude 1200 while the far end, having
heard our S1 perfectly well, concludes 2400. Each then demodulates the other
at the wrong rate, and V.22bis has no renegotiation to recover with. The
result is structured garbage and a retrain every few seconds until somebody
clears the call.

datamodem now notices and fixes it, without needing the far end to
cooperate:

```
145 of the last 444 characters arrived with the stop bit in the wrong place,
  which is what demodulating the far end at the wrong rate looks like
the link settled at 1200 having offered 2400 and will not hold, which means
  the two ends disagreed about the rate; re-offering 1200 only, so there is
  nothing left to disagree about
the framing is clean now, so re-offering 1200 was the right call
```

The test is the character framer's own opinion of what it is being given,
which makes it independent of what the far end was trying to say: a stream
demodulated at the wrong rate is noise, and noise puts the stop bit in the
wrong place about half the time, where real data over a poor line does not.
That needs a second framer with V.14 switched off, since V.14 rate adaption
exists precisely to tolerate stop bits that have moved and so reports no
framing errors at all.

The cure is to stop offering 2400. We then send no S1, the answering modem
has nothing to agree to and must stay at 1200, and the two cannot disagree —
which is exactly what `--bit-rate 1200` produces. It costs one retrain, only
on a call that was already failing. If the framing is still bad afterwards
the rate was never the problem, and the log says so rather than leaving the
wrong conclusion standing.

**The V.42 timers were armed before the rate was known.** spandsp derives
T400/T401/T403 and the detection window from the line rate and latches them
in `v42_restart()`. We called that while building the modem, when 2400 was
still only an offer. Settling at 1200 left every LAPM timer running twice as
fast as V.42 allows: T401 ran out of retries while the far end was still
well within its rights to be thinking, so the link died having trained
perfectly. The handshake is now armed in `note_connected()`, at the first
moment the rate is a fact rather than an offer. This is why fixing the rate
on the command line worked and letting it negotiate did not — a fixed rate
was the only way the timers ever matched the line, at either speed.

**spandsp came back from a dropout at the rate it offered,** not the one the
two ends settled on, re-sending S1 at a far end already in 1200 data mode.
Over RTP a jitter buffer underrun is routine, so this looped until somebody
cleared the call. Patched in the vendored source — see
`third_party/spandsp-v22bis/README.md`.

Both were invisible to the test suite for the same reason: two spandsp ends
make the same mistake at the same moment and stay in step. Only a real modem
on the far end, holding its rate, shows them up.

If a line still will not hold, `--bit-rate 1200` remains the thing to try:
offering only 1200 means there is no S1 to miss and nothing to disagree
about. It is a supported configuration — the test suite runs a full
V.42/V.42bis link at 1200 and at 2400.

### Establishment sent XID, never SABME

Once detection was working, real calls got one step further and stopped
again: four `T.401 expired` a second apart, then silence, then the far end
clearing the call. spandsp establishes the link by sending an **XID** —
parameter negotiation — where V.42 8.3 says a connection is established by
sending **SABME**. The far end waits for a SABME that never comes.

It fails silently too, because while configuring, the LAPM state is
`LAPM_IDLE`, which matches no case in the retry-exhausted switch, so not
even a disconnect is reported. Two spandsp ends never notice, because
spandsp answers its own XID. Fixed in the vendored source, which now calls
`lapm_connect()` — the function that sends SABME, and which already had its
own XID call commented out.

### The ADP gap is miscounted, so conformant peers get rejected

V.42's detection has the answering modem send an ADP: the character `E`, then
8 to 16 one-bits, then `C`, then 8 to 16 one-bits, repeated. spandsp counts
those ones with the increment on the wrong side of the test:

```c
case 2:
    s->neg.rxbits++;          /* the terminating zero is counted too */
    if (new_bit)
        break;
    if (s->neg.rxbits >= 8  &&  s->neg.rxbits <= 16)
```

so the accepted window is shifted by one — 7 to 15 ones, where V.42 says 8
to 16. Fed spandsp's own documented pattern:

```
    6 ones between E and C: no
    7 ones between E and C: DETECTED
   ...
   15 ones between E and C: DETECTED
   16 ones between E and C: no        <- legal, and refused
```

A far end using 16 is perfectly conformant and gets turned away. Detection
fails, we fall back to a link with no error correction, and the far end —
which has already committed to LAPM — carries on sending HDLC frames that
we read as line noise until somebody drops the carrier. The signature is a
stream of `ECEC...`, which is the ADP itself coming through the async framer:
the far end was answering all along.

`third_party/spandsp-v42/` holds the fix. It counts only the ones, and
widens the range to 6–24 — the pattern being matched is already two full
`E`/idle/`C`/idle cycles, so there is no realistic false positive, and a
wrong detection is cheap anyway because LAPM establishment then simply fails
and we fall back. `v42.c.orig` sits beside it; the whole change is a
`diff -u` away.

### The detection window has to fit the audio path, not a copper pair

V.42's detection timer T400 is 750 ms. That assumes a phone line, where the
far end's reply comes back more or less at once. Over RTP it does not — there
is a jitter buffer at each end — and the whole exchange has to fit inside the
window:

```
  far end recognises our ODP     ~1024 bit periods   0.43s at 2400 bps
+ its reply crosses the path     ~2 x jitter buffer  0.30s at the default 150ms
+ we recognise its ADP           ~1024 bit periods   0.43s
                                 ------------------------------------
                                                     1.15s, against a 0.75s window
```

So the specified window expires while the far end's ADP is still in flight.
We declare it non-V.42 and fall back; it, having committed to LAPM, carries
on sending HDLC frames that we read as line noise until one side gives up.
The signature is unmistakable once seen — a stream of `?~?~?~`, where `0x7e`
is the HDLC flag:

```
17:06:18.378  carrier established 2400
17:06:19.138  the far end did not answer V.42 detection    <- 0.76s later
17:06:19.158  falling back to a direct async connection
              ECECECEC...?~?~?~?~?~?~?~?~...
17:06:27.878  carrier lost, 953 bytes received
```

datamodem budgets the window for the path it is actually running over —
`750 ms + 2 x --jitter-buffer-ms`, plus two detector-recognition times at the
line rate. At 2400 bps with the defaults that is 1.9 s rather than 0.75 s.
Erring long is cheap: T400 governs only how long we keep listening, it never
makes the far end wait, and anything said meanwhile is held and delivered on
fallback rather than lost.

If it still happens, datamodem now says so rather than leaving you to decode
the hex, and `--v42-timeout` raises the outer bound.

### Falling back without losing the far end's first words

A host that does not speak V.42 starts sending the moment it has a carrier —
banner, login prompt, whatever. Meanwhile our V.42 detector is consuming
those same bits looking for an ADP that is never going to come. Unless
something is done about it, everything the far end said before we gave up on
V.42 is gone, and `--v42 detect` looks like it fails against exactly the
equipment it is supposed to fall back for.

So while V.42 is undecided the incoming bits are framed **twice**: once by
the detector, and once by the ordinary async framer, whose output is held
aside. If V.42 establishes, the held bytes were its own handshake and are
discarded. If we fall back, they were the far end talking all along and they
are delivered first, ahead of anything that arrives afterwards:

```
info [modem] the far end did not answer V.42 detection
warn [modem] the far end does not do V.42 (detection declined); falling back
info [modem] recovered 30 bytes the far end sent while V.42 was still being decided
```

The buffer holds 8 kB and fills from the front, which is more than ten
seconds of line time at 2400 bps — so what survives is the start of the
banner rather than the end of it.

Whatever is held back is only delivered if it reads as data. A far end that
is still running V.42 while this end has given up is sending HDLC, and a
parallel async framer will happily chop its flags into characters — which is
how a failed negotiation ends up looking like line noise on the terminal. A
stream of LAPM flags is a quarter `0x7e` or more once framed, and ordinary
text is not, so the two are easy to tell apart; HDLC is counted, discarded,
and reported rather than printed.

Falling back is triggered by whichever comes first: spandsp reporting that
detection found nobody, establishment failing with no time left on
`--v42-timeout`, or `--v42-timeout` expiring with spandsp having said nothing
at all — which happens on a sufficiently bad audio path, where it sits
silent indefinitely.

### Being more patient than spandsp about establishment

`--v42-timeout` used not to govern establishment at all. Detection would
succeed, the SABME would go out, the far end would refuse it with a **DM**
response, and spandsp would park in `LAPM_IDLE` — about 1.4 seconds after
the carrier came up, whatever the timeout was set to. Raising it changed
nothing, and the symptom was a terminal full of `?~?~?~` while the far end,
demonstrably alive and still in HDLC, streamed flags at a modem that had
stopped listening for them.

V.42 8.3.2.1 says a DM response to SABME means the far end is unable to enter
the connected state and that the originator must report the failure, so
spandsp is right to stop — but a far end entitled to say "not yet" is equally
entitled to be ready shortly afterwards. The fix is to keep asking:

```
V.42 establishment attempt 1 failed - the far end refused our SABME with DM;
  trying again in 1500ms (19.6s of --v42-timeout left)
V.42 establishment attempt 2: sending SABME again
...
V.42 establishment attempt 4: sending SABME again
error correction established protocol=V.42/V.42bis took_ms=5103
```

Retries go out every 1.5 s for as long as `--v42-timeout` leaves room for one
plus a T401 in which to be answered, and then it falls back. A far end that
answers DM every time, though, has made up its mind — after two refusals
datamodem stops asking and falls back in about three seconds rather than
spending the whole timeout confirming it. Silence still gets the full
timeout, because silence may be a path losing frames rather than an answer,
and the log now says which: every LAPM frame is logged in both directions at
`--log-level debug`, and frames arriving with a bad checksum are counted and
reported instead of being silently dropped.

### Taking "no, thank you" for an answer

Most of the time a far end that refuses establishment has already said so,
and we were not listening. Table 3/V.42 defines **two** answerer detection
patterns — `(E)(C)` for "V.42 supported" and `(E)(Null)` for "no
error-correcting protocol desired" — and §7.2.1.2 requires the originator to
act on which one arrived. spandsp matched both and initiated LAPM either way,
so a host that declined error correction during detection got a SABME anyway
and answered it with `DM`, exactly as it should. The symptom was detection
apparently succeeding, establishment being refused every single time, and a
screen full of HDLC.

datamodem now reads the pattern it was sent:

```
FLOW V.42 out ADP says no error-correcting protocol desired
the far end's detection pattern asked for no error-correcting protocol, so
  V.42 is not on offer here - falling back without asking
```

No SABME, no refusal, no waiting out `--v42-timeout`, and the session starts
in async immediately. `--log-level debug` prints `ADP says ...` either way,
so it is visible which of the two a given host sends.

If a host sends the "V.42 supported" pattern and *still* refuses every SABME,
there is nothing to be done from this end — it is contradicting itself.
`--v42 off` skips the detection phase altogether, which also stops the far
end's auto-reliable mode from starting, and gives a clean async session with
none of the waiting. Note also that
`--v42 require` and the fallback path now distinguish the two failures, which
are not the same problem: a far end that ignored detection does not do V.42,
while one that answered detection and then would not establish does.

Only the establishment step is repeated, not detection — a far end that has
already completed the detection phase will not run it again. That needs an
entry point spandsp does not export; see
`third_party/spandsp-v42/README.md`.

### V.42 and V.23 do not combine

`--v42` over V.23 is refused. One LAPM frame is 1072 bits, which takes 14
seconds to send on V.23's 75 bps back channel, while the far end's
acknowledgement timer is one second — every frame would be abandoned before
it finished transmitting. V.42 was written for symmetric modems at 1200 bps
and up; V.23 predates it and the combination never existed.

### V.42bis is negotiated, in the XID exchange

V.42bis has exactly one place where the two ends agree on it: the XID frame
the calling modem sends before it asks for the link with SABME, and the
answer that comes back. Whether to compress, in which directions, the
dictionary size and the longest string all go in it, and V.42bis is clear
that **without that exchange compression is off**, however either end was
configured.

spandsp got this wrong in three ways, all fixed in `third_party/spandsp-v42`:
it threw away the far end's XID and answered with its own fixed values; those
values were one-direction compression with a 512 codeword dictionary whatever
`--v42bis` said; and an earlier fix here, to get a bare SABME out to far ends
that never answered XID, stopped sending XID at all. So two datamodems
compressed at each other on trust, and a real modem that accepted the link
had agreed to no compression and received a stream it could not decode -
while some real modems, waiting for the XID that real modems send, refused
the SABME outright with DM.

Now the caller sends XID first, offering what `--v42bis`, `--v42bis-dict` and
`--v42bis-max-string` say; the answer carries the smaller of each and only
the directions both ends want; and that is what runs:

```
info [modem] V.42bis agreed: dictionary 1024, strings up to 16, both directions
warn [modem] the far end declined V.42bis in the XID exchange; error correction only
```

`--v42bis-dict` and `--v42bis-max-string` are therefore ceilings, not
something both ends have to match (the defaults, 2048 and 32, are what real
modems shipped with). A far end that never answers XID gets a SABME after two
of them, and an uncompressed link with a warning saying why - and if frames
were arriving damaged meanwhile, a second warning saying that it may have
answered after all.

**Some far ends compress without agreeing to.** A BBS's modem was seen
ignoring our XID entirely, accepting the SABME, sending its banner in the
clear - and then `00 00` and compressed data, which uncompressed is a screen
of garbage two lines in. V.42bis always starts in transparent mode and can
only leave it with its escape character, initially `00`, followed by a
command (`00`, `01` or `02`). So when the XID goes unanswered, what arrives
is delivered as it is but also fed to a decompressor in the background, to
keep its dictionary in step, and the first `00` settles it: followed by a
command, the far end is compressing, and from there on its data is
decompressed (`protocol V.42/V.42bis (receiving only)`, and a warning saying
so); followed by anything else, it is not, and the background decompressor
is dropped. Our own direction stays uncompressed, which a far end
decompressing anyway passes through untouched. The one case this misjudges
is a far end that is genuinely not compressing whose first `00` happens to
be followed by `00`, `01` or `02` - binary data, on a link with no V.42bis
on either side - which is why it only applies when the XID went unanswered.

**T401 is sized to the path.** V.42's acknowledgement timer is 1 s, which
outlasts a round trip on a phone line and not over RTP: one BBS was 772 ms
away, answered our XID a little after 1 s, and - having answered - went on
to compress, at an end that had given up waiting and was not decompressing.
The banner arrived clean for its first line, while V.42bis was still in its
transparent start-up, and then turned to garbage. T401 is now the round trip
(V.32 measures it; otherwise the two jitter buffers) plus half a second, plus
the time to send two of the longest frames at the line rate - ours, and one
of the far end's that its acknowledgement may be queued behind - never less
than V.42's 1 s. On the default jitter buffers at 9600 and up that is still
about 1 s; at 300 bps, where one frame takes three and a half seconds to
send, it is eight. It is worked out again whenever the rate changes. An XID
answer that does arrive late, before the link is up, is taken rather than
discarded. The V.42 detection window is sized from the same round trip. All
of it only makes this end more patient; nothing the far end does changes.
`--log-level debug` prints both ends' XID values and every LAPM frame.

**A one-way transfer was not being acknowledged.** spandsp's LAPM answered
every I-frame with an RR carrying the final bit, whether or not it had been
polled, and threw away any final response it had not asked for - its own
acknowledgements included. And it started T401 only when no timer at all
was running, which after the link came up was never, because the idle timer
T403 always was. With data going both ways nobody noticed, the
acknowledgements riding on I-frames instead; with data going one way - a
download - every acknowledgement was discarded, the window filled, and
nothing moved until T403 fired ten seconds later. A 100 KB transfer at 14400
had moved 59 KB after five minutes; it now takes one. Now F follows P, an unsolicited final response still
acknowledges, and T401 runs from the first unacknowledged frame, which
together put the same transfer at line speed. This was there all along, at
every rate.

**Received data is not dropped when the terminal falls behind.** The
receive queue holds 64 KB, which at the AT prompt - where nothing is read -
or into a slow pipe fills within seconds of a compressed download. Under
V.42 the far end is now asked to wait when it is nearly full (an RNR) and
let go once it has drained, and anything it sent meanwhile is asked for
again, so nothing is lost however long the wait. Upstream's
`v42_set_local_busy_status()` set the flag and told nobody. Without V.42
there is no way to ask, and the oldest data still goes, as it would on a
real modem.

**Detection traffic is real junk to a far end that is not listening for it.**
A calling modem's ODP pattern is DC1 characters - 0x11, and 0x91 with the
parity bit - chosen so that an async host would take them for XON, and one
that has never heard of V.42 sees a second or so of them (several seconds at
300 bps, where the window is longest) before the fallback. Real modems did
the same, which is why it is on by default anyway; `--v42 off` spares a host
that chokes on it. An answering modem sends nothing during detection, so an
async caller sees nothing at all.

The switch itself is clean: once detection gives up, the ODP character under
way is finished and the line held at mark - not the HDLC flags LAPM would
idle with - and two characters of mark go out before the first async one, so
the far end's framer is lined up for it.

### What it costs

The V.42 handshake takes about five seconds at 300 bps, on top of the answer
tone, so `CONNECT` arrives around ten seconds in rather than five. Against
that, compression can more than pay for itself. 968 bytes of repetitive menu
text over a real 300 bps call, all three byte-for-byte identical at the far
end:

| | on the wire | wall clock |
|---|---|---|
| direct async | 968 | 40s |
| V.42 | 968 | 45s |
| V.42 + V.42bis | 413 (2.34:1) | 27s |

### Knowing when it is safe to hang up

Handing a byte to the link layer is not the same as putting it on the line,
and with V.42 the difference is enormous: LAPM swallows the whole transmit
queue into its window at once and then spends the next half minute actually
transmitting it. Anything that decides when to clear a call therefore has to
ask `dm_modem_drained()`, not whether the queue is empty — the first version
of this cut 200 bytes off the end of every V.42 transfer, and a few bytes off
every async one.

spandsp can be asked how many frames LAPM still holds, but the answer is no
use here: its acknowledgement bookkeeping lags real delivery by as much as
ninety seconds, so waiting on it would hold a finished call open long after
the data arrived. The drain is estimated from the line rate instead, which is
what actually governs it, plus a tail for packetisation and the far end's
jitter buffer.

## Making the call work

The modem lives in the audio band, so everything between here and the far end
has to leave the audio alone.

- **G.711 only.** datamodem offers PCMU and PCMA and nothing else. Any
  compressed codec — G.729, Opus, GSM — is built around a model of the human
  voice and will destroy a modem signal completely.
- **No packet loss concealment, no VAD, no echo cancellation, no perceptual
  enhancement.** All four are switched off explicitly. PLC invents audio to
  cover a gap, which is exactly the wrong thing to hand a demodulator.
- **A fixed jitter buffer that never drops or stretches a frame**
  (`--jitter-buffer-ms`, default 150). Chasing latency by discarding samples
  corrupts the stream the demodulator is tracking. Bigger costs round-trip
  delay that an interactive session can feel; smaller starves the receiver.
- **Media ports** stay inside `[--rtp-port, --rtp-port + --rtp-port-range]`.
  Open exactly that range; a narrower allowance shows up as a call that
  connects and then never trains.

### Call setup

Nothing in the modem runs until the call is actually answered. That sounds
obvious and is easy to get wrong: pjmedia's conference bridge starts clocking
our media port the moment it joins, which is well before the far end picks
up, and a `183 Session Progress` with SDP gives a live audio path carrying
ringback. A modem left running through that spends the ringing counting down
its answer-tone wait against ringing tone, trains on whatever noise is there,
and announces a carrier that never existed — after which it fills the screen
with one repeated character. So the modem is armed only when media is active
*and* the call is confirmed, and `datamodem dial` does not return until both
are true.

If a demodulator does end up locked to nothing, the symptom is unmistakable
once you know it: a screen of `U` (0x55), or some other single byte, broken
every fifty characters or so by a framing slip. datamodem watches for it —
if one byte value accounts for 90% of a 256-byte window it says so once,
rather than letting it look like a bad line:

```
warn [session] 100% of the last 256 bytes from the far end were 0x55. That is
what a demodulator that never locked produces, not data - check --modulation
matches what is answering, and that nothing is transcoding the audio.
```


The answering end announces itself with a 2100 Hz answer tone (V.25) before it
starts training. The calling end has to sit out that tone: 2100 Hz is close
enough to the answering modem's band that a receiver will lock onto it,
declare itself trained and then drop the moment the tone stops.

So the caller waits `--answer-wait` seconds (default 5) before training —
enough to cover a full-length tone, which V.25 allows to run 2.6 to 4 seconds.
If it *hears* the tone, it extends the wait to `--answer-tail-ms` after
hearing it. Hearing the tone never shortens the wait: spandsp's detector
reports the tone as a momentary event rather than a level, and over RTP it
often does not fire at all, so the fixed wait is what actually protects the
handshake. Being late costs nothing — the answering modem sits in its training
pattern until someone answers it.

Bell 103 skips all of this. It has no answer tone; the answering modem simply
raises its 2225 Hz mark carrier, so there is nothing to wait for.

### When the carrier drops

A carrier that disappears is not immediately the end of the call. Real lines
glitch and a modem pair will retrain in under two seconds, so datamodem stops
treating what arrives as data, waits four seconds, and only then reports
`NO CARRIER`. A retrain inside that window is logged and the session carries
on.

## Testing it

```
./build/datamodem selftest                    # two modems in memory, no SIP
./scripts/loopback-test.sh                    # a real call over real RTP
./scripts/escape-test.sh                      # +++ over a real call
./scripts/exec-test.sh                        # answering for a program, three calls
./scripts/pty-test.py                         # the same, on a real terminal
```

The last three take flags for the link layer, so the same call can be driven
over each of the three protocols (V.42/V.42bis is the default):

```
CALL_FLAGS="--no-v42bis" ./scripts/loopback-test.sh      # V.42 alone

DM_FLAGS="--v42 off" ./scripts/pty-test.py                # direct async

# an answering end with no V.42: the caller's fallback path
ANS_FLAGS="--v42 off" \
    EXPECT_CALL="falling back to a direct async connection" \
    ./scripts/loopback-test.sh
```

`selftest` runs two modems back to back through the same 20 ms frame cadence
pjmedia uses, pushes 512 bytes each way and compares them byte for byte. It
needs no network and no credentials, and it is the fastest way to tell a
modulation problem from a SIP problem. Try it on each modulation:

```
for m in v34 v32bis v32 v22bis v22 v23 v21 bell103; do ./build/datamodem selftest --modulation $m --no-step-down; done
for v in "--v42 off" "--no-v42bis" ""; do
    ./build/datamodem selftest $v
done
```

By default the line between them is perfect. `DATAMODEM_SELFTEST_LINE`
makes it worse, which is how V.32's echo canceller and round-trip
measurement get exercised without a telephone line:

```
# 150 ms each way, the far end's echo back 10 dB down, noise, G.711
DATAMODEM_SELFTEST_LINE="delay=150,echo=-10,noise=-40,ulaw" \
    ./build/datamodem selftest --modulation v32

# the 16-point fallback every 9600 bps V.32 modem has to support
DATAMODEM_V32_NO_TRELLIS=1 ./build/datamodem selftest --modulation v32

# 20 ms of loud noise every 3 seconds: damaged frames for V.42 to recover
DATAMODEM_SELFTEST_LINE="delay=150,echo=-10,ulaw,burst=20/3" \
    ./build/datamodem selftest --modulation v32bis --v42 require
```

`drift=100` makes the answerer's clock 100 ppm fast as the caller hears it,
and `cut=20/600` silences the line both ways for 600 ms, 20 seconds in:

```
# a dropout long enough to need a retrain
DATAMODEM_SELFTEST_LINE="ulaw,delay=100,cut=15/2000" DATAMODEM_SELFTEST_BYTES=80000 \
    ./build/datamodem selftest --modulation v34 --v42 require
```

V.34 has its own matrix, about forty cases covering every symbol rate,
carrier, pre-emphasis filter and trellis code, echo, drift, dropouts,
retrains and renegotiation, and its own unit tests for the bit-exact parts:

```
./scripts/v34-selftest.sh
./build/v34test                # or ctest --test-dir build
```

and hooks that force what probing would otherwise choose:

```
DATAMODEM_V34_SYMBOL_RATES=0x08    # bit 0 = 2400 ... bit 5 = 3429; here 3000 only
DATAMODEM_V34_CARRIER=high         # or low
DATAMODEM_V34_PRE_EMPHASIS=5       # 0 to 10
DATAMODEM_V34_TRELLIS=64           # what our receiver asks for: 16, 32 or 64
DATAMODEM_V34_SHAPING=1            # ask for expanded shaping
DATAMODEM_V34_RENEGOTIATE=2:14400  # caller renegotiates down 2 s in; ":answer" for the answerer
```

Stepping down has a matrix too: a modem at its defaults calling and
answering every older one, over three lines, with and without V.42; pairs
that both step down from different places; and each of V.42's fall backs -
a far end that will not compress, one with no V.42 at all - at each
modulation. About 140 cases, a couple of minutes:

```
./scripts/step-down-test.sh
```

It is built on `DATAMODEM_SELFTEST_FAR`, which makes one end of the selftest
an older modem — one modulation, no stepping down — or, with `:auto`, one
that steps down from there too:

```
DATAMODEM_SELFTEST_FAR=answer:v22bis ./build/datamodem selftest          # V.34 calls a V.22bis modem
DATAMODEM_SELFTEST_FAR=call:bell103 ./build/datamodem selftest           # a Bell 103 modem calls V.34
DATAMODEM_SELFTEST_FAR=answer:v34:auto ./build/datamodem selftest --modulation v21
```

and `DATAMODEM_SELFTEST_V42`, which changes one end's link layer:

```
DATAMODEM_SELFTEST_V42=answer:off ./build/datamodem selftest            # no V.42 there: async
DATAMODEM_SELFTEST_V42=call:no-v42bis ./build/datamodem selftest        # V.42 without compression
DATAMODEM_SELFTEST_V42=answer:require ./build/datamodem selftest
```

A few more test hooks, none of them options:

```
# more data than 512 bytes, to see error rates 512 bytes cannot
DATAMODEM_SELFTEST_BYTES=200000 ./build/datamodem selftest --modulation v32bis

# data from the caller only, the way a download goes: acknowledgements
# cannot ride on I-frames, so LAPM has to get its RRs right
DATAMODEM_SELFTEST_ONEWAY=1 ./build/datamodem selftest --modulation v32bis --v42 require

# the answerer stops reading for 60 seconds, two seconds in: the receive
# queue fills, and V.42 has to hold the caller off without losing a byte
DATAMODEM_SELFTEST_STALL=60 DATAMODEM_SELFTEST_ONEWAY=1 DATAMODEM_SELFTEST_BYTES=200000 \
    ./build/datamodem selftest --modulation v32bis --v42 require

# V.32bis rate renegotiation, which a clean line never needs: the calling
# end asks for 9600 five seconds in ("5:9600:answer" for the answering end)
DATAMODEM_V32_RENEGOTIATE=5:9600 DATAMODEM_SELFTEST_BYTES=50000 \
    ./build/datamodem selftest --modulation v32bis --v42 require

# and the same against an answering end that ignores it: the request times
# out, both ends retrain, and neither asks again
DATAMODEM_V32_IGNORE_RENEGOTIATION=answer DATAMODEM_V32_RENEGOTIATE=5:9600 \
    DATAMODEM_SELFTEST_BYTES=50000 ./build/datamodem selftest --modulation v32bis --v42 require
```

The V.32bis fallback needs one end that is not V.34, which `selftest` cannot
do; over a real call it can:

```
MODULATION=v32bis CALL_FLAGS="--modulation v34" ./scripts/loopback-test.sh
MODULATION=v32bis ANS_FLAGS="--modulation v34" ./scripts/loopback-test.sh
```

`loopback-test.sh` starts an answering datamodem and dials it over real SIP
signalling and real RTP on the loopback interface, which additionally covers
SDP, codec negotiation, the jitter buffer and the conference bridge.
`escape-test.sh` drives a real call through `+++`, `ATI` and `ATH` and asserts
that the three plus signs did not reach the far end.

`pty-test.py` is the only test that covers raw mode — the shell tests all pipe
their input, so `isatty()` is false and none of the terminal handling runs. It
puts datamodem on a real pty, checks that `ECHO`, `ICANON`, `ISIG` and `OPOST`
are actually off while connected and actually back on afterwards, and drives
`+++`, `ATI`, `ATO`, `+++` and `ATH` through it - with the full screen on, as
it is on any terminal; `DM_FLAGS=--no-tui` runs it the plain way.

`screen-test.py` points a hostile far end at the full screen: a BBS screen in
colour and CP437, then everything that could damage a real terminal - escape
sequences for the title, the character set, the alternate screen, mouse
reporting, a full reset, scroll regions and cursor movement onto the status
line - and 3 KB of random bytes. It checks that none of it reached the pty,
and replays what did through a terminal emulator to check the screen itself.
It needs `pip install pyte`, and skips without it. `DM_FLAGS=--no-tui` points
the same far end at the plain output instead, which needs no pyte.

`term-fuzz` feeds the emulator random bytes and sequences at random window
sizes, resizing as it goes, under the address and undefined-behaviour
sanitizers:

```
cmake --build build --target term-fuzz && build/term-fuzz 1 30
```

## Exit codes

| | |
|---|---|
| 0 | the call ran and was cleared normally |
| 2 | bad flags |
| 3 | missing credentials or nonsense settings |
| 4 | SIP transport or registration failure |
| 5 | call rejected, busy, or never answered |
| 6 | answered, but the link never carried data — never trained, or `--v42 require` and the far end does not do V.42 |
| 7 | a deadline was hit |
| 8 | internal error |

## How it fits together

```
  terminal (raw mode)                                    PSTN
        |                                                  ^
   stdin | stdout                                          |
        v                                                  |
  +------------------+   bytes    +-------------+   G.711  |
  | session.c        |<---------->| modem.c     |<-------->+
  |  escape detector |  tx/rx     |  spandsp &  |   RTP
  |  AT interpreter  |  queues    |  v32.c pumps|
  +------------------+            +-------------+
                                        ^
                                        | pjmedia_port, 20 ms frames
                                  +-------------+
                                  | sip.c       |
                                  |  pjsua      |
                                  +-------------+
```

- `modem.c` — the data pump. spandsp's FSK and V.22bis engines, our V.32 and V.32bis,
  the answer tone, the asynchronous character framing, and the V.42/V.42bis
  stack. The
  transmit framer is written out by hand rather than using spandsp's
  `async_tx`, because `async_tx` signals end-of-data as soon as its queue
  empties and tells the pump to drop the carrier — right for a fax burst,
  fatal for an interactive session that is idle most of the time. A real
  async modem holds the line at mark between characters, which is all the
  difference amounts to.

  When V.42 is on it replaces that framing entirely, so the pump's `get_bit`
  and `put_bit` are switches rather than the framer itself and falling back
  to direct async is one flag rather than rewiring the pump. The layering,
  outermost first, is `terminal ↔ V.42bis ↔ LAPM ↔ data pump ↔ line`.
  Compression happens inside the callback LAPM uses to ask for a frame, and
  every burst is flushed: without the flush the compressor sits on a typed
  command indefinitely, waiting for input that will not arrive until the far
  end replies to the thing stuck in the buffer.
- `v32.c` — V.32 and V.32bis, complete: transmitter, receiver (matched
  filter, Gardner timing, fractionally spaced equaliser, Viterbi decoder for
  the trellis code), echo canceller, the start-up and retrain procedures, and
  V.32bis's rate renegotiation. It knows
  nothing of spandsp or pjmedia - samples in, samples out, bits through
  callbacks.
- `sip.c` — pjsua: registration, call setup, and the `pjmedia_port` that pulls
  20 ms of modem output and pushes 20 ms of the far end back in.
- `session.c` — the loop that joins the terminal to the modem, the escape
  detector and the AT interpreter.
- `term.c` — the full screen: an ANSI-BBS emulator the far end's bytes are
  fed through, so that only what it draws reaches the terminal, CP437 and
  UTF-8 decoding, the status line, and the plain-output sanitizer for when
  the screen is not ours.
- `ring.c` — the two byte queues across the thread boundary. The transmit
  queue holds about four seconds of line time, and not polling stdin when it
  is full is the whole of the flow control that way; the other way, the
  64 KB receive queue holds the far end off through V.42 when it fills.
- `config.c`, `log.c`, `tty.c`, `util.c` — options, logging, raw mode, and the
  small shared pieces.

Threading is the same shape as faxmodem: pjmedia owns a media thread that
calls the port every 20 ms, the main thread owns the terminal, and the two
meet only at the queues. Received bytes wake the main loop through a self-pipe
rather than being polled for.
