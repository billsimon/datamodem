# datamodem: development notes

What was learned building datamodem: how it is put together, why each modem
works the way it does, the bugs found in spandsp along the way, the
measurements behind its thresholds, and the test hooks. For how to build and
use it, see the [README](README.md).

Much of this was written as it happened, so a section sometimes describes a
problem first and the fix after it; the fix is in the code in every case.

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
- `v34.c`, `v34_codec.c`, `v34_info.c`, `v34_dsp.c` — V.34; see its section
  below for what lives where.
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
- `hayes.c` — `datamodem modem`: a Hayes modem on a serial port, its command
  state and control lines; see "Terminal programs" below.
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

`datamodem` borrows its structure from its sibling project
[`faxmodem`](../faxmodem), which does the same thing for T.30 fax: the option
handling, the logging, the pjsua setup and codec discipline are deliberately
the same shape.

**A slow log cannot hurt the call.** Lines from the media and SIP threads are
queued and written by a thread of their own, so a log that stops accepting
writes - a terminal over a bad link, a pipe nobody is reading - costs log
lines (counted, and said so when it recovers) rather than audio. Before
this, a debug log into a full pipe stalled the modem mid-frame and dropped
the call.

**The speaker** (`--speaker`) runs on its own clock alongside the call's
audio, so turning it on and off leaves the samples the modem sees unchanged.
Ringing the network sends as early media (a 183) plays as it is; a plain 180
carries no audio, so datamodem makes the North American ringing tone itself.
Only the playback side of the sound device is opened.

**The terminal** goes into raw mode with `ISIG` and `OPOST` off, so ctrl-C
goes down the line. The full screen exists because a far end's bytes passed
straight to a terminal - line noise, a binary file, a hostile BBS - can switch
its character set, change its modes or title, move its scroll region or
overwrite the status line, and some of that outlives the program.

**`--exec` and `--exec-tty`.** The command's process group gets `SIGHUP` when
the caller hangs up. With `--exec-tty` the pseudo-terminal is the command's
stdin, stdout, stderr and controlling terminal, in a session of its own, so a
shell's jobs get `SIGHUP` as they would on a real line; its speed is the
connect rate or the fastest standard one below it. Flow control: datamodem
reads the program's output only as fast as the line takes it, and stops
taking the caller's bytes from the modem when the program falls behind, which
V.42 then passes back to the caller. All calls under `--calls` share the one
registration, so there is no gap between calls while it registers again.

## Modulations

All eight carry data, verified byte for byte in both directions by
`selftest` and over a real call by `loopback-test.sh`.

The FSK frequencies in the README's modulation table are mark/space. V.21
and Bell 103 number their channels in opposite directions — V.21 channel 1
is the calling modem's, Bell 103's is the answering modem's — and spandsp
follows each convention faithfully, which is an easy way to end up with both
modems transmitting into each other's band. Nothing in the test suite can
catch that, because both ends of every test are this same program and invert
together; that table is checked against the standards rather than against a
passing test.

### Telling the far end's modem apart

**Calling**, datamodem listens for what the answering modem sends before it
has heard anything it recognises, and answers in kind:

| It hears | Which means | So it runs |
|---|---|---|
| ANSam (2100 Hz, amplitude modulated) — decided after one second of the tone, from a clean 15 Hz sine about 20% deep on its envelope | V.8 | V.34, once the far end answers CM with JM |
| a plain answer tone | no V.8: an older modem | V.32bis — AA once it has heard a second of the tone |
| what sounded like ANSam, and then no JM | an older modem whose answer tone had some amplitude modulation on it by the time it arrived | V.32bis, silent until it hears AC — and on hearing USB1 or a V.21 carrier during V.8, stop CM and go to it |
| AC (600 + 3000 Hz) | V.32 | V.32bis |
| USB1, V.22's unscrambled ones | V.22bis or V.22 | V.22bis, at once — not after Annex A's Tc > 3.1 s, which a real 2400 bps modem, offering USB1 for three seconds and then V.21, outlasted (as Annex A's own Note 1 warns) |
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

A V.21 caller is only taken once its carrier has been offered: a real one
says nothing until then, and what is in its band before that is something
else - V.8's CM, which is V.21 channel 1 too. A Bell 103 caller is taken
whenever it is heard, answer tone included, because a real one may well
speak first (below), and nothing else a caller sends sits at 1270 Hz.

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

V.21, Bell 103 and V.23 wait to hear the far end's tone before they connect,
at either end, rather than taking any energy for a carrier — a V.32 caller's
AA, or the far end's echo of our own, used to be enough. For V.23 the tones
are the marks, 1300 Hz forward and 390 Hz back. A calling V.23 modem that
took any energy for a carrier once "connected" to a V.8 answerer's USB1:
2250 Hz is close enough to V.23's 2100 Hz space that the demodulator read
continuous space, and passed on 0x00 at 120 bytes a second for most of a
minute. A calling FSK modem holds its own carrier at mark for half a second
plus the path before it passes data, so the answerer is listening when the
first character arrives.

The answering one is connected the moment it hears that carrier, so the
caller is the one not yet listening when the answerer has something to say
first - a BBS's banner. V.22 and V.22bis are the same way round: the caller
finishes training most of half a second after the answerer. Over V.42 that
costs nothing, since LAPM comes up only once both ends are. With no V.42
there is nothing to send it again, and Bell 103 callers received "OME TO THE
TEST BBS" for "WELCOME TO", V.23 ones "EST BBS", V.22bis ones "BBS". So an
answering modem with no V.42 holds mark for a second after it connects
before it sends anything; what it was given waits in the queue.
`scripts/async-answer-test.sh` checks it.

A calling Bell 103 or V.23 modem goes on air before it has heard the
answering carrier: on hearing an answer tone (2100 Hz) or USB1 (2250 Hz) it
raises its own mark, though it still connects only once it has heard the far
end's. A real Bell 103 caller's receiver takes anything between about 2025
and 2225 Hz for the answering carrier, and both of those are in it, so real
ones do the same — and automode answerers depend on it. A Cisco MICA answers
with its answer tone, USB1, V.21's 1650 Hz and V.23's 1300 Hz in turn, and
never offers 2225 Hz unprompted: it listens for the caller's 1270 Hz instead.
A Bell 103 caller that waited for 2225 Hz, as ours used to, sat silent until
the MICA hung up. For the V.23 caller this keeps what it always did, going
on the air as the answer tone ends; only with no answer tone or USB1 at all
does it now wait for 1300 Hz.

A calling V.21 modem stays silent until it hears 1650 Hz, as the real ones
do: V.21's answering band, 1650 to 1850 Hz, does not include 2100 Hz. Against
the MICA, which offers V.21 explicitly, that works. Speaking first was tried
against the Level 29 BBS, whose V.8 modem, hearing no CM, plays ANSam and
then 46 seconds of USB1 and nothing else: it answers a Bell 103 caller
during the USB1, but ignored 38 seconds of V.21's 980 Hz. V.21 cannot reach
it at all, and nothing else has needed a V.21 caller to speak first.

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

**Dropouts that arrive as audio.** That last was lost *packets*, which
`dm_v32_rx_fillin()` hears of and freezes adaptation across. A dropout on the
far end's own line arrives as perfectly good RTP with next to nothing in it,
and nothing froze for that. As the far end goes quiet, and again as it comes
back, the equaliser's window holds almost no signal, and its update -
normalised by what the window holds - is enormous: the taps are wrecked in a
few symbols and the SNR goes below zero and stays there until a retrain. A
production V.32bis call showed exactly that signature, every few minutes, at
every rate:

```
info  [v32] reception has been poor (-9.6 dB SNR) for 2.0 seconds; retraining (tag=in)
```

An SNR below zero is not noise - the error is bigger than the signal - and
in the selftest a hundred milliseconds of silence (`cut=20/100`) was enough
to do it and cost a six-second retrain, where V.34, which holds its own
adaptation while the far end is quiet, rode out 200 ms without one. Now a
window 6 dB down on what it usually holds in data stops all adaptation
(equaliser, phase, timing, echo canceller) until it has filled with signal
again, and V.32 rides out 100, 200 and 350 ms with no retrain; the 400 ms
that counts as the carrier gone still retrains. `scripts/retrain-test.sh`
checks it.

**Against other manufacturers' modems.** Everything above is datamodem
against datamodem, which has exactly the weakness the V.42 sections below
describe: two copies of the same code agree with each other even where they
are both wrong. It has since been run against three real far ends over a SIP
trunk - a Cisco MICA and two BBSes - and works with all three, with V.42 and
V.42bis; most of what is in the V.42 sections below is what those calls
turned up. Before that, the parts most likely to matter against real
hardware were checked against the Recommendation itself rather than
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

- **down**, when reception has been poor for a second, to the best rate the
  receiver would bear now - which is what V.32 can only retrain for. A
  second is enough to tell, and by two a line going bad has often gone too
  far for it; where reception has all but gone, there is a retrain at two
  seconds as before;
- **up**, when reception has been good enough for a higher rate, with a dB
  to spare, for ten seconds: after a step down that was more cautious than it
  needed to be, or a bad patch that passed. One rate at a time, whichever end
  asks (our R4, or our R5 answering the far end's): SNR at one rate is only
  an estimate of the next, and less of one for each rate beyond, and a
  production call whose hold on 12000 lifted went straight from 9600 to
  14400 - the rate that had failed soonest. Each rate now holds for the
  thirty seconds before the next step up may be asked for. An attempt that
  gets nothing - the far end's receiver would not have it - waits twice as
  long before the next.

```
info  [v32] reception has been poor (18.3 dB SNR) for 1.0 seconds; asking the far end to change to 7200 (tag=out)
info  [v32] changed rate to 7200 (trellis coded) without retraining: SNR 17.8 dB (tag=out)
...
info  [v32] reception is good enough for 12000 (31.5 dB SNR); asking the far end to change to 12000 (tag=out)
info  [v32] changed rate to 12000 (trellis coded) without retraining: SNR 29.9 dB (tag=out)
```

The Recommendation sends the rate change in the middle of whatever was
crossing, so the bits in flight at that moment are lost: V.42 retransmits
them, and without V.42 a character or two goes astray, exactly as with a
retrain. A far end that does not answer a request to step *up* - the
Recommendation warns that some V.32 modems use the marking bit for something
else - or answers it and does not finish, is retrained with instead, and not
asked again; its own requests are still answered. One that does not answer
a step *down* is retrained with but asked again next time: a step down is
asked for with reception going, and the far end's may be going with it, so
no answer says nothing about what the far end is. (It used to count the
same, and a call whose line went bad once was left with nothing but retrains
for the rest of it.)

**Remembering a rate that failed.** A BBS in production logged this, over
and over, through an eleven-minute call that then dropped:

```
info  [v32] trained at 9600 (trellis coded): SNR 28.6 dB, round trip 186 ms
info  [v32] reception is good enough for 14400 (36.2 dB SNR); asking the far end to change to 14400
info  [v32] changed rate to 14400 (trellis coded) without retraining: SNR 22.5 dB
... a few minutes ...
info  [v32] reception has been poor (1.9 dB SNR) for 2 seconds; retraining
```

The far end had left 14400 out of its rate signal at training, which 6.1
recommends be done by "likely receiver performance"; datamodem stepped up to
it ten seconds into every stretch at 9600 anyway, and three times out of
three it did not last. Training is a few hundred milliseconds of a quiet
line, and a line whose trouble comes and goes trains as well as a good one,
so each retrain chose afresh and the step up followed. Now a rate that fails
within five minutes of starting is avoided, and everything above it, for
five minutes the first time, twenty the second and eighty after that - out
of what we offer at a retrain (R1, R2, R3) and what we ask or answer for in
a renegotiation (R4, R5), which are ours to choose. Notes 1 and 3 to 8/V.32
bis ask much the same of a renegotiation that came to nothing: that it be
"taken into account in determining the desirability of further rate
renegotiation". Failed means:

- our reception gave out at it, stepping down or retraining;
- the far end retrained, or asked for less, there;
- on probation after a step up - our own or the far end's - the new rate's
  SNR stayed short of what it would have been offered at for a second,
  starting half a second in. The step up takes the SNR at the old rate as
  the measure of the new, which holds where the trouble is noise and need
  not where it is anything else; probation steps straight back in a fraction
  of a second rather than waiting for the line's next bad moment to turn it
  into a retrain. (The SNR logged with `changed rate` is taken 24 symbols
  in, and reads well below the line even on a clean one - 28 dB on a 39 dB
  line in the selftest - so the 22.5 dB above says less than it seems to.)

```
info  [v32] changed rate to 14400 (trellis coded) without retraining: SNR 22.6 dB (tag=out)
info  [v32] 14400 did not hold - reception did not bear it; staying below it for 5 minutes (tag=out)
info  [v32] reception at 14400 is short of it (24.9 dB SNR); asking the far end to change to 12000 (tag=out)
```

Not every failure is the rate's fault, though. A line that goes away for a
few seconds takes every rate with it, and holding that against each rate in
turn walks the call down to 4800 one bad patch at a time - which the first
version of this did, taking 340 seconds over a transfer the old code made in
200. So a failure counts against a rate only where something says a lower
one would have done better - reception still good enough for the next rate
down, the far end asking for less, the far end giving up while our own
reception was fine - or where the rate was a step up from what training
settled on, which is a bet, and a lost one; and not at all if the rate ran
five minutes first. Over a line that fades four seconds in thirty,
`scripts/retrain-test.sh` measures it: where a step down will do, 157
seconds against 177 and no retrain; where nothing survives, the same
retrains as before and 186 seconds against 198.

V.32 alone does none of this: it has nothing between 9600 and 4800 and no way
back up, so one bad patch would leave a call at half speed for good.

A smaller fault turned up on the way. Answering a request, we said what we
wanted (R5) by the SNR at that moment - measured against the far end's
preamble and rate signal, which read about 10 dB low even on a clean line,
and turned down every step up the far end asked for that came near the
margin. It is the SNR the request interrupted now.

Datamodem against datamodem it has run fifteen minutes at 14400 without a
bit error, with the far end's echo 10 dB down, a 150 ms path, G.711 and the
far modem's clock 50 ppm off ours; it holds 14400 from -100 to +200 ppm.
The constellations are checked point by point against the Recommendation's
figures (and agree with spandsp's V.17, which shares them), and the rate
signals against its tables; it has since connected at 14400 to the same three
real far ends as V.32.

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

**When reception goes bad.** Reception poor for a second is answered with a
rate renegotiation, not a retrain: 11.6 offers it as a way "to resynchronize
the receiver without going through a complete retrain", because the far
end's TRN in it retrains our equaliser and our MP asks for what the receiver
then measures. A second or so, against five or more for Phase 2. Below 12 dB
our receiver is not following the far end well enough to hear its S, so
there it waits two seconds and retrains as before.

Each direction also remembers a rate that failed, the way V.32bis does (see
there for what counts as failing, and why not every failure does): our
receive rate when our reception gives out, and our transmit rate when the
far end's does - it asks for less, or retrains while our own reception was
fine. What goes in our MP is ours to choose, and the rate each way is the
most both MPs allow (11.4.1.1.3), so that is where the limit goes.

V.34 had no way back up, either: a call that stepped down, or whose
renegotiation's TRN met a bad patch and settled low, stayed there to the
end. Now, as in V.32bis, reception good enough for the next rate in, with a
dB to spare, for ten seconds asks for it by renegotiating, one rate at a
time, and the far end may come up one in the same exchange if its own
reception says so; an attempt that gets nothing waits twice as long. A
limit lifting renegotiates the same way, one rate up in whichever direction
it held back. Over a line with one bad patch six minutes in, the call that
used to finish at 26400 climbs back to 31200 within a minute.

The transmit side is what a production BBS needed. A caller whose own
receiver could not hold 28800 from us retrained every fifteen seconds and
asked for 28800 again each time, four times in two minutes, until
`--max-retrains` hung up on a call each of whose retrains had worked:

```
info  [v34] the caller is sending tone B; answering a retrain (tag=in)
info  [v34] trained: 28800 bit/s out at 3429 symbols/s on 1959 Hz, 31200 bit/s in ...
... four times ...
error [session] the link retrained 4 times at 31200 bps and will not hold (40.1 dB SNR)
```

Now our transmitter steps below a rate the far end gives up on, and
`--max-retrains` counts only retrains that did not come back slower in
either direction, afresh after two quiet minutes. `DATAMODEM_V34_PLAIN_RECOVERY`
(under Testing) makes one end of the selftest a far end like that one; the
other end steps from 33600 to 31200 after its first retrain and holds there,
where it used to answer nine retrains in the same transfer.

Not every far-end retrain is the rate's fault. A call from the MICA through
a VoIP path retrained three times in four minutes as the round trip each
Phase 2 measured went 128, 148 and 288 ms - a jitter buffer somewhere past
Telnyx taking on frames, each one splicing audio into the signal, which no
rate survives (at 3429 baud and 1959 Hz, 20 ms is not a whole number of
symbols or carrier cycles; V.32 bis, at 2400 and 1800, rides through). Each
retrain held a rate against our transmitter all the same. A retrain from
data now remembers the rate memory as it was before the failure went in,
and when Phase 2 measures the round trip, puts it back if that has moved by
15 ms or more (`dm_path_watch_t`, used by V.32 bis too):

```
info  [v34] the round trip went from 200 ms to 240 across that retrain: the path changed under the call, which no rate would have held through, so not holding it against the rate (tag=selftest-answer)
```

That call's third retrain is the one that lost it: six times over, two
seconds apart, we reversed tone A and the caller never reversed B in reply.
Each time we went silent for 70 ms and started over with tone A, which gave
the caller 50 ms of A before each reversal - the least 11.2.1.2.3 allows,
and maybe less than its tone detector needed to be listening for one.
11.2.2.2.2 says otherwise: no reversal of B in 2 s, keep A on and reverse it
again. We do that now, five times before starting over
(`the caller has not answered our reversal of tone A in 2 seconds; reversing
it again`). The caller's side of it, 11.2.2.1.3, was already near enough:
silence, then tone B and a wait for A's reversal, which an answerer doing
11.2.2.2.2 will be sending.

Testing that recovery turned up two more. Phase 2's tone detector judged a
tone by how much of the band's power was steady, with no floor on the power
itself: in an answerer's retrain, the caller's 70 ms of silence before tone
B - the tail of its data signal dying away in it - read as tone B, then as
B's reversal, and the round trip came out at nothing, so the probing window
was in the wrong place and the retrain failed. Below 4 x `pmin` (-42 dBm0)
there is now no tone and no reversal.

And a renegotiation we started chose our receive rate from the previous
renegotiation's TRN: `rn_begin()` cleared `far_trn_on` but not the count of
the far end's TRN symbols, which still passed "400 of them" before any had
arrived. A call that renegotiated over a bad patch asked for the bad
patch's rate at every renegotiation after, and the deaf far end of
`DATAMODEM_NO_RETRAIN` sat at 16800 with 37 dB of SNR. With that fixed,
every renegotiation really measures, and on a line with a burst of noise
every two seconds the bursts landed in the TRN and walked the call down to
2400. So the rate is chosen from TRN in 25-symbol blocks, leaving out any
more than 6 dB worse than the median (`dm_robust_mean()`), and a
renegotiation waits for 600 symbols of the far end's TRN rather than 300,
so that a 60 ms burst covers less than half of it. Over six burst lines the
transfers now take less time on four, the same on one, and longer on one;
on one of them the old code lost the call.

A far end that retrains over a packet of ours it lost - the MICA did, twice,
each loss in its RTCP reports within 2.5 seconds of a retrain - has not
shown the rate too high either. The session passes each new reported loss
to the modem (`dm_modem_far_loss()`), and a far-end retrain up to 3 seconds
after one, or 8 before, is not held against the rate (`dm_loss_watch_t`,
V.34 and V.32bis):

```
info  [v34] the far end reports losing some of our packets around its retrain 0.3 seconds ago, which is what it retrained over and no rate would have held through; not holding it against the rate (tag=selftest-answer)
```

Not yet done: V.34 changes symbol rate only by retraining, so a retrain
whose probing meets a fade settles at 2400 symbols/s and stays there until
the next retrain, however good the line is after. On the selftest's line
with a deep three-second fade every forty, every retrain now completes
inside the fade and the call runs at 21600 throughout.

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

**What it has been tested against.** First, datamodem against itself: over
the selftest's simulated line at every symbol rate, both carriers, all
pre-emphasis filters and all three codes, through G.711, with echo down to
-6 dB, round trips to 700 ms, noise, bursts, dropouts, and clocks to
±100 ppm apart; and over a real SIP call on loopback, at 33600 with V.42.
Because both ends of every test are this code, the parts that have to be bit
exact with somebody else's modem - the superconstellation, the shell
mapper's tables, the trellis encoders, the framing of every rate - are
checked against the Recommendation's tables and against spandsp's
independent tables (`tests/v34_test.c`), not just against each other.

The paths only a real modem exercises are V.8 interop, and the precoder,
non-linear encoder, expanded shaping and 32/64-state codes on transmit, since
our own receiver never asks for them. It has since connected to a Cisco MICA
and two BBSes, one of which asked for all of those at once. Those calls
found three V.34 bugs, all fixed: a type 0 MP during renegotiation wiped the
precoding coefficients; CJ's last octet went out without its stop bit and
the silence after it was short, because the end of V.8 was timed in receive
time, a frame behind; and a far end's spurious tone A reversals (set off by
our repeated INFO0c) were taken for the real one, so a reversal now counts
only once L1 follows it. (The same calls showed that a lost LAPM link should
hang up rather than fall back to async.) Long round
trips - one BBS is 945 ms away - are where Phase 2 is most fragile. Run those
calls with `--log-level debug`: every phase logs what it heard and why it
decided what it did, and `DATAMODEM_RECORD` (under Testing) keeps the audio.

V.90 is a different machine again, and nothing here implements it.

## V.42 and V.42bis

What follows is how V.42 detection, LAPM and V.42bis came to work against
real modems. Almost all of it is the same story: two copies of spandsp agree
with each other even where both are wrong, so each of these only showed up
against real equipment. The patches are in `third_party/spandsp-v42`, whose
README lists them one by one.

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

### No V.42 over V.21, Bell 103 or V.23

`--v42 require` over V.23 is refused. One LAPM frame is 1072 bits, which
takes 14 seconds to send on V.23's 75 bps back channel, while the far end's
acknowledgement timer is one second — every frame would be abandoned before
it finished transmitting. V.42 was written for symmetric modems at 1200 bps
and up; V.23 predates it and the combination never existed.

V.21 and Bell 103 used to run V.42 like everything else, and between two
datamodems it works (see "What it costs" below). Against real equipment it
gains nothing and costs a lot: a Cisco MICA answering at V.21 does not
answer V.42 detection, and at 300 bps the detection window is nearly eight
seconds, so a host without V.42 - which is most hosts at that speed - gets
over a hundred DC1s before the fall back. So `carries_v42()` in `modem.c`
now says no for all three: a call that starts on one runs `detect` as `off`,
and one that steps down to one drops its V.42 state when the carrier comes
up, before anything has gone through it. `--v42 require` is refused up front
for the three, and hangs up on a call that steps down to one.

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
that has never heard of V.42 sees a second or so of them before the
fallback (several seconds at 300 bps, which is why V.42 no longer runs
there). Real modems did
the same, which is why it is on by default anyway; `--v42 off` spares a host
that chokes on it. An answering modem sends nothing during detection, so an
async caller sees nothing at all.

The switch itself is clean: once detection gives up, the ODP character under
way is finished - even if the fall back to async comes first, which it can a
frame later; until that was handled, 0x91 went out cut short as 0xf1 - and
the line held at mark - not the HDLC flags LAPM would
idle with - and two characters of mark go out before the first async one, so
the far end's framer is lined up for it.

### What it costs

V.42 no longer runs at 300 bps (see above); this was measured while it did.
The V.42 handshake took about five seconds at 300 bps, on top of the answer
tone, so `CONNECT` arrived around ten seconds in rather than five. Against
that, compression can more than pay for itself. 968 bytes of repetitive menu
text over a real 300 bps call, all three byte-for-byte identical at the far
end:

| | on the wire | wall clock |
|---|---|---|
| direct async | 968 | 40s |
| V.42 | 968 | 45s |
| V.42 + V.42bis | 413 (2.34:1) | 27s |

### Giving up on a link the far end was still running

A V.32bis call to a BBS in production ended like this, nine seconds after
its third retrain:

```
error [modem] the error-corrected link was lost - our DISC was acknowledged, so the call is no use (tag=in)
```

Both halves of that were misleading. What had happened was that our polls
went unanswered N400 times, and LAPM gave up and sent DISC - but the far end
*acknowledged* the DISC, so it was alive and still running LAPM: we hung up
on it. And the reason it gave was what became of the DISC, which overwrote
the reason it was sent.

spandsp's N400 is 5, so a link gives up a little over five seconds of T401
after the last answer. V.42 sets no value (9.2.2: "no default value is
specified ... minimum value of 1") and its Appendix III.2 advises "a
relatively large value (e.g. 16), such that repeated attempts ... shall be
made over a span of several seconds". Five seconds is less than a bad patch
and the retrain that cures it: the timers are frozen during a retrain itself
(7.10's L-SUSPEND - they count transmitted bits, and none are transmitted),
but the second or two of poor reception before it count, and so does the
time the far end takes to come back after. Once the link is up, N400 is now
16; establishment keeps spandsp's 5, which datamodem already repeats for as
long as `--v42-timeout` allows. And a DISC answered, or not, no longer
replaces the reason it was sent:

```
error [modem] the error-corrected link was lost - the far end stopped answering our polls, so the call is no use (tag=in)
```

`build/v42test` runs two LAPM links over a bit pipe and cuts it: ten seconds
without answers is ridden out, which N400 of 5 was not, and twenty-five is
given up on with that reason.

**A far end that loses us and does not notice.** With N400 at 16 a later
call on the same BBS ran twenty-five minutes, and ended the same way: the
far end stopped answering, for the seventeen seconds V.42 now allows, while
everything we heard from it was perfect - no poor reception, no tone, no
lost packets. Its receiver had lost our signal, and it never retrained, so
nothing at the modem level on our side could tell. V.42's unanswered polls
were the only sign. So once five in a row have gone unanswered
(`dm_v42_unanswered()` in the vendored V.42), and our own reception has been
undisturbed - no retrain, rate change or dropped carrier - for eight
seconds, the modem trains the far end's receiver again: V.32 by retraining,
which the far end must join on hearing AC or AA (7.1 and 7.2/V.32bis), V.34
by a renegotiation at the rates in use, whose TRN goes both ways (11.6) and
which becomes a retrain if the far end does not hear it. V.42's timers stop
meanwhile (7.10/V.42), so the eleven polls left are still there after it;
if they go unanswered too, the link is given up as before.

```
warn  [modem] the far end has not answered 5 polls in a row, though we hear it fine - it has most likely lost our signal (tag=in)
info  [v32] retraining to bring its receiver back; retraining (tag=in)
```

The eight quiet seconds matter. Without them, a line with a burst of noise
every two seconds - polls lost, but to the line, which the pumps already
deal with by renegotiating - had V.34 retrain on top, through a Phase 2 the
bursts kept breaking, and the call was lost where renegotiations alone had
carried it. V.32bis and V.34 retrain when their own reception is poor
anyway; this is only for the far end's.

A V.34 call from abroad stopped answering after losing two of our packets,
renegotiated with us when we asked, and closed the link half a minute
later - and nothing said whether it had heard us again in between. Now the
modem says when the far end answers again (`the far end is answering
again, 7.2 seconds after we retrained`), and if five more polls go
unanswered after the first try, with the same eight quiet seconds, it tries
once more: a retrain, all of Phase 2, for V.34 (`dm_v34_retrain()`), and
another retrain for V.32bis. After that the polls left run out as before.

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

## Call setup

**Why the media path is set up the way it is.** datamodem offers only PCMU
and PCMA, and switches off packet loss concealment, VAD, echo cancellation
and perceptual enhancement explicitly: PLC invents audio to cover a gap,
which is exactly the wrong thing to hand a demodulator. The jitter buffer is
fixed and never drops or stretches a frame, because chasing latency by
discarding samples corrupts the stream the demodulator is tracking.

What a fixed buffer cannot do is make two clocks agree. We send a packet per
20 ms of our clock and the far end one per 20 ms of its own; if they differ,
a buffer somewhere - ours, or the carrier's - slowly drains or fills, and
each time it runs dry or overflows the modem on the other side of it gets a
20 ms gap or splice. A V.32bis call to a production BBS retrained every few
minutes at every rate while the round trip it measured at each retrain crept
up a frame at a time, 107 ms to 367 over half an hour - about 135 ppm, and
about one frame's worth of drift between retrains - and nothing in the log
said what the buffer was doing. The session now does:

```
info  [session] the jitter buffer had nothing for 20 ms and threw away 0 ms - the modem was handed a gap (it holds 150 ms now, 150 on average, 140 to 160); the far end has sent 4 packets fewer than we have since the connect (80 ms of audio)
info  [session] audio path at retrain 3: jitter buffer holding 150 ms (150 on average, 140 to 160), empty for 20 ms and 0 ms thrown away since the connect; round trip 287 ms; the far end has sent ...
info  [session] audio path: jitter buffer holding 160 ms (160 on average, 160 to 160), empty for 0 ms and 0 ms thrown away since the connect, 0 packets lost; the far end has sent as many packets as we have since the connect
```

(The last is a real one, from a loopback call; the first two show the form.)

- a line each time the buffer has nothing to give or throws frames away, at
  most every ten seconds, so it can be set beside the modem's own;
- at each retrain, the buffer and the round trip the retrain measured, which
  on a path whose delay is not changing should not change either;
- every five minutes, and in the end-of-call `audio path` summary
  (`jb_avg_delay_ms`, `jb_max_delay_ms`, `jb_depth_ms`, `jb_empty_ms`,
  `jb_discard_ms`, `far_extra_packets`), the buffer and how many more packets the far end has
  sent than we have since the connect, which no jitter buffer can disguise.
  A count that grows through the call is the two clocks drifting apart; one
  that jumps and stays is the path adding or taking out audio; one either way
  is only where the two ends' ticks fall. It used to be given as our clock in
  ppm, which made a jump look like drift fading as the call went on: a MICA
  call's `-454 (+/-114)` at three minutes and `-300` at four and a half were
  the same 4 packets, 80 ms, arriving at the moment the caller retrained.

pjmedia counts the buffer in the codec's frames, and G.711's are 10 ms, not
a packet's 20: its `size`, taken for 20 ms frames, once read 280 to 320 ms on
a loopback call with 150 ms in each buffer, and the empty and discard counts
were logged at twice what they were. `size` times 10 ms is what it holds
now; its average, least and most delay are over the whole stream, and on a
long call say little about the moment.

### Doing without a jitter buffer

A fixed buffer only ever grows. A packet late enough to empty it puts a
frame of nothing in front of everything after it, and a packet more than our
clock asks for stays in it until it overflows. A V.34 call to a BBS through
a carrier that added a packet or two every few minutes - +13 over 39 minutes,
in steps, each one around a burst of poor reception, and none on another
caller's route the same afternoon - went from 20 ms of buffer to its 240 ms
limit, the round trip from 248 ms to 588; once it was full each packet more
was thrown away, a splice made by us, and the last landed in the middle of a
retrain and ended the call.

A buffer exists to give a sound card a sample every 125 us. A modem needs no
such thing: it recovers the far end's clock from the signal, and can take its
samples whenever they come. So `--jitter-buffer-ms 0` puts a pjmedia
transport adapter under the stream (`on_create_media_transport`) that copies
each RTP packet, decoded, into a small queue (`dm_rtp_rx_t`) on its way to
the stream - which still keeps the statistics, answers RTCP and feeds the
speaker, but no longer the modem - and the modem port empties the queue on
each tick of the bridge's clock, ahead of sending its own 20 ms. Packets go
in sequence-number order: one out of order waits for the one before it, up
to 40 ms (`DM_DIRECT_HOLD_MS`); one lost becomes as many samples of nothing
as the packet after it holds; and with nothing arriving at all, after 40 ms
the modem is given nothing a tick at a time, so its clock runs on as it did
through an empty buffer. That is owed: the packets that come after a stall,
late, are skipped until it is paid off, so a stall leaves no delay behind,
and a path that has become slower for good costs one gap.

The first version placed each packet by its RTP timestamp instead, which is
exact to the sample - and on the first call with a datamodem calling through
another carrier, every packet toward the caller came to be behind where it
expected them, none lost and none out of order (`late_packets=762
gap_ms=15380 rx_lost=0`): whether the path became slower for good or the
carrier's timestamps went back, the queue gave the modem nothing for each
tick and moved on at our clock, the packets never caught up, and the caller
heard nothing for the rest of the call. pjmedia's jitter buffer goes by
sequence number too.

Timestamps now only say how much is missing between two packets given one
after the other. The same carrier, Sinch, took 20 ms at a time out of what
it passed toward the caller - 7 times in 8 minutes, numbering its packets on
as if it had not, so the caller received 11 packets fewer than it sent - and
the caller's reception gave out at nearly every one: what came after the gap
arrived a fraction of a symbol out of step, which no equalizer follows. Up
to 200 ms (`DM_RTP_RX_MAX_SKIP`) ahead of where the packet before left off,
that much nothing now goes between them, and the receiver rides through it
as through a packet lost. A jump back, or further ahead - a new timeline,
not a gap - is only counted. Both are logged (`ts_jumps`, `ts_skipped_ms`).

The modems compare what they hear with what they sent by counting samples
each way (V.34's round trip is `n - ntx`), and with a buffer the two counts
advance together. Without one, the count received follows the far end's
clock, so extra packets from the far end still move the round trip the
modem measures - the same splice in the signal, just no longer a delay
anyone waits through. On loopback the round trip measures 0 ms where two
150 ms buffers made it 300. V.42's allowance for modulations that do not
measure one still assumes 150 ms in the far end's gateway.

What the far end's RTCP receiver reports say of our packets - how many it
lost, the jitter it saw, the network round trip - goes with the five-minute
and retrain lines, a line of its own whenever it reports more lost, and the
summary (`far_reports`, `far_lost`, `far_jitter_ms`, `net_rtt_ms`): the
direction we send in is the one we cannot measure, and the one a far end
goes deaf in.

The next long call on that BBS answered the question: `clock_ppm=+13`, to
within 13 - a packet or so - over twenty-five minutes. The clocks agree. Its jitter buffer did
run dry once, for six frames, at a moment two packets were lost - which a
V.32bis call before the dropout fix would have retrained over, and this one
rode out. The round trip's creep is more likely jitter buffers refilling
after each such hiccup than drift.

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

## Terminal programs

A BBS terminal program - SyncTERM, the one this was built against - wants the
line and nothing else: it renders the far end's screen and runs the file
transfers itself. There are three ways to give it one: as a command it runs,
as a network connection, and as a modem on a serial port.

**`--stdio`** is for a program that runs a command and talks to its stdin and
stdout, which is SyncTERM's Shell connection. It is a pty there, so everything
that keys off `isatty()` - the full screen, the filter that keeps a far end's
escape sequences off a real terminal, result codes going to stderr when stdout
is redirected - has to be told the terminal is not a person's. `dm_tty_set_line()`
is that switch. The result codes go on the line, since that is where a
terminal program looks for a modem's words; the log does not, apart from
errors, because stderr is the same pty. Before the call, quiet mode keeps
typing off the screen and turns `OPOST` off too, or the pty's `ONLCR` makes
every result code `CR CR LF`.

SyncTERM's Shell connection on Windows is ConPTY, which is a console emulator
rather than a pipe: it renders what passes through into a screen buffer and
sends a VT description of that screen on, so bytes are not preserved and
`--stdio` cannot work there.

**`datamodem rlogin-listen`** is what Windows has instead. A SyncTERM entry's
address is only a host and a port, but RLogin's handshake (RFC 1282)
carries two user names, which SyncTERM fills from the entry's Username and
Password: the number to dial rides in one of them. Nothing else in RLogin
reaches the data unless the server asks - window-size reports follow an
urgent `0x80` from the server, and `0x10`/`0x20` switch flow control - so a
server that never sends urgent data has a clean 8-bit pipe both ways, and
SyncTERM starts RLogin in raw mode. Telnet would have meant `IAC` escaping in
both directions, and SyncTERM has no RFC 2217, so a Telnet connection cannot
carry DTR or a dial string either. `rlogin.c` reads the handshake, answers it
with the NUL the RFC asks for, and `dup2()`s the socket onto stdin and stdout,
as `datamodem modem` does its port; the session is `--stdio`'s, line mode and
all, with `hangup_on_eof` set because the connection closing is the hangup.
Before the call is up the session does not read stdin, so while it dials a
thread watches the socket and abandons the call when it closes; during
training a close is only noticed at `CONNECT`, which then hangs straight up.
There is one SIP call at a time, so while one is up another thread accepts
whatever else connects and sends it away. The connection is closed with a
`shutdown()` and a moment's wait for the client, because closing a socket
with unread input sends a reset that can overtake the `NO CARRIER` before it.

**`datamodem modem`** is for a program that wants a modem on a serial port.
`hayes.c` opens the port and
`dup2()`s it onto stdin and stdout, so the session - which talks to stdin and
stdout - runs on it unchanged. Its own state is the command line: AT commands
are parsed as a Hayes did, waiting for `A`, then `T` or `/`, then everything to
S3, with chained commands and everything else that init strings for other
modems send accepted and ignored. Result codes all go through one hook, the
session's included, so that `V`, `Q` and `X` apply to every one.

The control lines follow a null-modem cable's wiring: our DTR is the
terminal's DCD and DSR, and its DTR is our DSR. So `&C1` drives DTR with the
carrier, and the session asks `dtr_dropped()` each pass, which reports a fall
from high to low once; a DTR that was never raised never hangs anything up.
While `ATD` waits in `dm_sip_dial()`, a thread watches the port, and a
keystroke or DTR falling abandons the call through the stop flag, as on a
Hayes. The port's flow control defaults to none, not the `&K3` a real modem
has: SyncTERM on Windows drops RTS when its own flow control is off, and a
modem honouring CTS would then never answer.

SyncTERM's Modem connection is strict about what it reads back, and
`scripts/modem-test.py` reads the way it does: control characters before a
line are skipped, a line ends at CR, one containing what was just sent is the
echo, and the next line must contain `OK` - or, after `ATDT`, `CONNECT`, within
60 seconds. Anything else is a failure, which is why `RINGING` is said under
`--stdio` and never on the port.

A pty has no modem-control lines (`TIOCMGET` and `TIOCMBIS` are `ENOTTY`), so
the tests cannot cover DCD or DTR, and SyncTERM's Modem connection refuses a
pty outright: it raises DTR before anything else and gives up when that fails.

## Testing

```
./build/datamodem selftest                    # two modems in memory, no SIP
./scripts/loopback-test.sh                    # a real call over real RTP
./scripts/escape-test.sh                      # +++ over a real call
./scripts/exec-test.sh                        # answering for a program, three calls
./scripts/async-answer-test.sh                # a banner said at once, over direct async
./scripts/exec-tty-test.sh                    # the same on a pseudo-terminal (--exec-tty)
./scripts/pty-test.py                         # the same, on a real terminal
./scripts/stdio-test.py                       # --stdio on a pty, as SyncTERM's Shell runs it
./scripts/rlogin-test.py                      # rlogin-listen, connected to as SyncTERM's RLogin does
./scripts/modem-test.py                       # datamodem modem, dialled as SyncTERM's Modem does
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

`fade=4/30/-32` raises the noise to -32 dBm0 for four seconds in every thirty,
from fifteen seconds in: a line whose trouble comes and goes, which training
never sees. `fade=.../call` and `noise=-36/call` put it only on what the
caller hears (`/answer` the answerer), for a direction one end's receiver
cannot hold and the other's can. `DATAMODEM_V34_PLAIN_RECOVERY=call` makes
that end retrain whenever its reception is poor and remember no rate that
failed, as many far ends do; `DATAMODEM_NO_RETRAIN=call` makes it never do
anything about its own reception at all - a far end that loses us and does
not notice, which only the other end's V.42 can see. `slip=40/40/call` makes
what the caller hears 40 ms later from forty seconds in, repeating the 40 ms
before - a jitter buffer on the path taking on two frames - and
`DATAMODEM_V34_SLOW_TONE=call:100` makes the caller miss a reversal of tone A
that comes less than 100 ms after the tone, as a slow tone detector might.
`lose=22/call` loses the 20 ms packet the caller would hear 22 seconds in
and tells the answerer two seconds later, as an RTCP report would; and
`DATAMODEM_V42_DEAF=call:20/18` makes the caller's V.42 hear nothing for 18
seconds from 20 seconds after the connect, though its modem hears us fine -
a far end the first try does not bring back.
`scripts/retrain-test.sh` runs the cases these
were made for - a step down where one will do, no rate held against a
dropout, a step up that does not hold, a far end that keeps retraining - and
checks what the log says happened, not just that the data arrived:

```
./scripts/retrain-test.sh
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

`DATAMODEM_RECORD=path` records a real call's audio from the moment it is
answered, to `path.<tag>.wav`: stereo at 8 kHz, what we heard on the left and
what we sent on the right. A log says what datamodem concluded; the
recording says what the far end actually sent.

```
DATAMODEM_RECORD=/tmp/call ./build/datamodem 5551234 --log-level debug --log-file call.log
```

`DATAMODEM_REPLAY=call.wav` plays such a recording back: one calling modem,
against the left channel, instead of two modems back to back. The far end
cannot react to anything new, so past the first exchange the two drift
apart, but everything up to it - which answer tone it was, when CM went out,
what came after it, what we stepped down to - plays out exactly as it would
have, with the time into the recording at which each happened:

```
DATAMODEM_REPLAY=/tmp/call.out.wav ./build/datamodem selftest
```

`scripts/replay-test.sh` replays every recording in `recordings/` (kept out
of git) and checks each still links up - in the modulation its name starts
with, as in `v22bis-2400-synchronet.wav`.

A real 2400 bps modem's call, replayed like that, is how the ANSam decision
came to be made after one second of tone rather than spandsp's two and a
half: CM then starts at 3.5 s into the call instead of 5.0, with 2.6 s of
the far end's ANSam left to hear it in rather than 1.1.

And `DATAMODEM_SELFTEST_V42`, which changes one end's link layer:

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
