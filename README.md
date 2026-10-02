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

There is no modem hardware and no sound card anywhere in this. spandsp's data
pumps are wired straight into a pjsip media port, so the modulated audio is
the RTP stream.

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
or from a `--config` file as `key = value`. Command line wins, then the
environment, then the file. `datamodem help` lists all of them;
`examples/datamodem.conf` is a commented starting point.

```
export DATAMODEM_PASSWORD=...
datamodem +15551234567 --server sip.example.com --username 1001

# a host that wants 7E1 and speaks Bell 103
datamodem 5551234 --server sip.example.com --username 1001 \
    --modulation bell103 --data-bits 7 --parity even

# unattended: pipe a command in, collect the reply, hang up
echo -e 'help\r' | datamodem 5551234 --server sip.example.com --username 1001
```

### stdout is the line, stderr is the diagnostics

While a call is up, stdout carries only the bytes that came off the line, and
everything datamodem has to say about itself goes to stderr. So:

```
datamodem 5551234 > session.txt        # a clean transcript of the far end
datamodem 5551234 --log-file dm.log    # a clean terminal
```

The modem result codes — `CONNECT`, `NO CARRIER`, `OK` — and the `AT`
conversation are local, not remote, so they follow you: stdout when stdout is
your terminal, stderr when it has been redirected somewhere.

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

## What it can actually do

| `--modulation` | Standard | Rate | Calling end transmits | Answering end transmits |
|---|---|---|---|---|
| `v22bis` | ITU-T V.22bis | 2400 or 1200 | 1200 Hz carrier | 2400 Hz carrier |
| `v22` | ITU-T V.22 | 1200 | 1200 Hz carrier | 2400 Hz carrier |
| `v23` | ITU-T V.23 | 1200 down, 75 up | 390/450 Hz | 1300/2100 Hz |
| `v21` (default) | ITU-T V.21 | 300 full duplex | 980/1180 Hz | 1650/1850 Hz |
| `bell103` | Bell 103 | 300 full duplex | 1270/1070 Hz | 2225/2025 Hz |

All five carry data, verified byte-for-byte in both directions by `selftest`
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
genuinely comfortable. Against that, V.22bis takes about 6 seconds to train
where V.21 takes about 4, and QAM is far less tolerant of a poor audio path
than FSK is — if a trunk is doing anything at all to the audio, the 300 bps
modes will survive it and V.22bis will not.

The default is still `v21`, because 300 bps will get through an audio path
that nothing else will, and thirty characters a second is a perfectly usable
interactive terminal — it is how everyone did this in 1982. Reach for
`--modulation v22bis` when you want the speed and the line is good.

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

### What about 9600 and above?

Achievable, but only half duplex, and it would change the shape of the
program. spandsp has no V.32, V.32bis, V.34 or V.90 — those are the
full-duplex high-rate standards, and no free implementation of them exists.
What it does have are the fax modems, and V.29 is a genuinely working 9600
bps data pump:

```
  V.29   9600 bps   trained 0.26s   1024/1024 bytes   *** DATA OK ***
  V.29   7200 bps   trained 0.26s   1024/1024 bytes   *** DATA OK ***
  V.29   4800 bps   trained 0.26s   1024/1024 bytes   *** DATA OK ***
  V.17  14400 bps   trained 1.34s   1024/1024 bytes   corrupt
```

V.29 carried 1024 bytes byte-for-byte at every rate it offers, and trains in
a quarter of a second. V.17, which would reach 14400, is corrupt at every
rate — its author's own note says the symbol and carrier syncing is not good
enough, and that still holds.

The catch is that V.29 is half duplex: one direction at a time, carrier up,
burst, carrier down, turn around. Using it would mean a turnaround protocol
deciding who may transmit, and roughly a third of a second of dead air on
every reversal. That is fine for moving a file and poor for typing at a
prompt, so it is not a drop-in replacement for the duplex modes — it is a
different mode of operation that the session loop would have to grow. Nothing
here implements it today.

## Error correction and compression

V.42 (LAPM) and V.42bis are both implemented, on top of any modulation, and
both are **off by default**:

```
datamodem 5551234 --v42 detect             # error correction
datamodem 5551234 --v42 detect --v42bis    # and compression
```

| `--v42` | |
|---|---|
| `off` (default) | direct async: start and stop bits, no error correction |
| `detect` | run the V.42 handshake; fall back to direct async if the far end does not answer |
| `require` | run the handshake; give up on the call if the far end does not answer |

With V.42 running there are no start and stop bits on the line at all — the
bit stream is HDLC frames, retransmitted until they arrive intact. `--data-bits`,
`--parity` and `--stop-bits` stop meaning anything, because there is no
character framing left for them to describe. `CONNECT` says which you got:

```
CONNECT 300                  # direct async
CONNECT 300 V.42             # error corrected
CONNECT 300 V.42/V.42bis     # error corrected and compressed
```

`--v42bis` requires `--v42`, and is refused without it. Compression on an
uncorrected link is worse than no compression: both ends build a shared
dictionary as they go, so a single corrupted byte desynchronises them and
everything after it is garbage rather than one bad character.

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
specified. datamodem sets the rate before `v42_restart()` — after it is too
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

### Two things spandsp still does not do

**Its XID exchange does not negotiate the V.42bis parameters.** The dictionary
size and maximum string length it advertises are compile-time constants that
the handshake never updates, so there is nothing to agree on — **both ends
must be configured to match**, or the dictionaries diverge and the session
turns to noise:

```
--v42bis-dict 2048           # codewords, 512-4096
--v42bis-max-string 32       # 6-250
```

The defaults are 2048 and 32, which is what real modems shipped with and
about three times better than the 512/6 that spandsp's own XID advertises.
If you are talking to third-party equipment that follows the XID, set
`--v42bis-dict 512 --v42bis-max-string 6` on our side to match.

**Detection traffic is real junk to a far end that is not listening for it.**
The ODP pattern will show up as perhaps a hundred garbage characters before
the fallback happens. Real modems had the same problem, and it is the reason
`--v42` is off by default here rather than on.

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
./scripts/pty-test.py                         # the same, on a real terminal
```

The last three take flags for the link layer, so the same call can be driven
over each of the three protocols:

```
CALL_FLAGS="--v42 detect --v42bis" ANS_FLAGS="--v42 detect --v42bis" \
    ./scripts/loopback-test.sh

DM_FLAGS="--v42 detect --v42bis" ./scripts/pty-test.py

# V.42 offered by the caller only: the fallback path
CALL_FLAGS="--v42 detect --v42-timeout 8" \
    EXPECT_CALL="falling back to a direct async connection" \
    ./scripts/loopback-test.sh
```

`selftest` runs two modems back to back through the same 20 ms frame cadence
pjmedia uses, pushes 512 bytes each way and compares them byte for byte. It
needs no network and no credentials, and it is the fastest way to tell a
modulation problem from a SIP problem. Try it on each modulation:

```
for m in v22bis v22 v23 v21 bell103; do ./build/datamodem selftest --modulation $m; done
for v in "" "--v42 detect" "--v42 detect --v42bis"; do
    ./build/datamodem selftest $v
done
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
`+++`, `ATI`, `ATO`, `+++` and `ATH` through it.

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
  |  escape detector |  tx/rx     |  spandsp    |   RTP
  |  AT interpreter  |  queues    |  data pump  |
  +------------------+            +-------------+
                                        ^
                                        | pjmedia_port, 20 ms frames
                                  +-------------+
                                  | sip.c       |
                                  |  pjsua      |
                                  +-------------+
```

- `modem.c` — the data pump. spandsp's FSK and V.22bis engines, the answer
  tone, the asynchronous character framing, and the V.42/V.42bis stack. The
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
- `sip.c` — pjsua: registration, call setup, and the `pjmedia_port` that pulls
  20 ms of modem output and pushes 20 ms of the far end back in.
- `session.c` — the loop that joins the terminal to the modem, the escape
  detector and the AT interpreter.
- `ring.c` — the two byte queues across the thread boundary. The transmit
  queue holds about four seconds of line time, and not polling stdin when it
  is full is the whole of the flow control.
- `config.c`, `log.c`, `tty.c`, `util.c` — options, logging, raw mode, and the
  small shared pieces.

Threading is the same shape as faxmodem: pjmedia owns a media thread that
calls the port every 20 ms, the main thread owns the terminal, and the two
meet only at the queues. Received bytes wake the main loop through a self-pipe
rather than being polled for.
