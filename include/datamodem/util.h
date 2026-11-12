#ifndef DATAMODEM_UTIL_H
#define DATAMODEM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Monotonic milliseconds, for deadlines and the escape guard timer. */
int64_t dm_now_ms(void);

/* The clock without any simulated offset, for measuring how long something
 * really took. */
int64_t dm_now_real_ms(void);

/* Moves dm_now_ms() forward. For the selftest, which generates audio far
 * faster than real time and would otherwise see a frozen clock. Nothing on a
 * real call touches this. */
void dm_clock_advance_ms(int64_t ms);

/* mkdir -p on the directory part of a file path. Returns false and fills err
 * on failure. */
bool dm_mkdir_parent(const char *path, char *err, size_t err_len);

/* "1h02m03s", for the session summary. */
void dm_format_duration(int64_t ms, char *out, size_t out_len);

/* Renders one byte the way a terminal log should show it: printable ASCII as
 * itself, everything else as <XX>. out needs 8 bytes. */
void dm_format_byte(int c, char *out, size_t out_len);

/* Parses a control-character spec for --escape-key: "^]" / "0x1d" / "29" /
 * "none". Returns false on a bad spec; *out is -1 for "none". */
bool dm_parse_key_spec(const char *s, int *out);

/* Splits a SIP name-addr, such as
 *
 *     "Alice Smith" <sip:+15551234@trunk.example.com;user=phone>
 *
 * into its display name ("Alice Smith") and the user part of its URI
 * ("+15551234", which on a phone call is the number). A bare URI, sips: and
 * tel: URIs are understood too. Either part is empty when absent. Control
 * characters are dropped from both, since they are destined for a log line
 * and another program's environment. */
void dm_parse_sip_party(const char *s, char *name, size_t name_len, char *user, size_t user_len);

/* The rates that have failed a modem on this call, so that it does not go
 * straight back to one: a retrain picks its rate from how well training
 * went, and training is short and quiet where a bad line's trouble is
 * neither. A rate that fails within five minutes of starting, and every
 * rate above it, is avoided for five minutes the first time, twenty the
 * second and eighty after that. One that ran longer before failing was
 * not too fast - the line had a bad moment, which any rate would have
 * met - and is not held against it. Times are line time, in samples at
 * 8000 a second. */
typedef struct
{
    int failed;               /* stay below this; 0 = no limit */
    long long until;          /* line time the limit lifts */
    unsigned count;           /* failures held against a rate so far this call */
} dm_rate_memory_t;

/* rate, in use since line time since, failed at line time now. Returns how
 * many seconds it will be avoided for, or 0 if it ran long enough that the
 * failure is not held against it. */
double dm_rate_memory_failed(dm_rate_memory_t *m, int rate, long long since, long long now);

/* The rate to stay below at line time now, or 0 for no limit. */
int dm_rate_memory_limit(const dm_rate_memory_t *m, long long now);

/* "staying below it for 5 minutes", or why not: for a log line, given what
 * dm_rate_memory_failed() returned. */
void dm_rate_memory_text(double hold_s, char *out, size_t out_len);

/* A retrain from data is about to hold a failure against a rate - the far
 * end's retrain, against what we send, or ours, against what we receive.
 * The round trip the retrain measures says whether the rate was to blame: if
 * it has moved by DM_PATH_MOVED_MS or more, the path changed under the call -
 * a jitter buffer along it growing or shrinking, which splices audio into
 * the signal or takes it out - and no rate would have held through that. A
 * production V.34 call to a MICA retrained three times as its round trip
 * went 128, 148 and 288 ms, and held a rate against our transmitter each
 * time. Arm with the memories as they are before the failure goes in (b may
 * be NULL); judge with the new round trip, which puts them back and returns
 * true if it moved and the failure had changed them. Round trips in ms, < 0
 * for not measured. */
#define DM_PATH_MOVED_MS 15.0

typedef struct
{
    bool armed;
    double rtd_ms;
    dm_rate_memory_t a, b;
} dm_path_watch_t;

void dm_path_watch_arm(dm_path_watch_t *w, double rtd_ms, const dm_rate_memory_t *a, const dm_rate_memory_t *b);
bool dm_path_watch_judge(dm_path_watch_t *w, double rtd_ms, dm_rate_memory_t *a, dm_rate_memory_t *b);

/* A far-end retrain from data that held a failure against what we send it,
 * and the far end's RTCP reports of losing our packets: a loss reported
 * around the retrain means a gap in what it heard is what sent it there,
 * which no rate would have held through. A MICA call lost two of our
 * packets and retrained twice, each loss reported within 2.5 seconds of a
 * retrain, and held 31200 and then 28800 against our transmitter for it.
 * The two come in either order - the reports are five seconds or so apart -
 * so a loss reported up to DM_LOSS_BEFORE_S before the retrain counts, or
 * up to DM_LOSS_AFTER_S after. Arm with the memory as it is before the
 * failure goes in, then check; and report each loss. Either puts the
 * memory back and returns true if the failure had changed it. Times are
 * line time, in samples at 8000 a second. */
/* The mean of n blocks' squared errors, leaving out those more than 6 dB
 * worse than the median - a burst of noise in a short training signal,
 * which says nothing of the rate the line will carry between bursts. A
 * line with a 60 ms burst every two seconds had it land in renegotiation
 * after renegotiation, and was walked from 33600 down to 2400 by them. On
 * a line steadily that bad no block is left out. n at most 64; 0 if n is
 * 0. */
double dm_robust_mean(const double *blocks, int n);

#define DM_LOSS_BEFORE_S 3
#define DM_LOSS_AFTER_S 8

typedef struct
{
    bool armed;
    long long retrain_at;
    long long loss_at;        /* the last loss reported, 0 if none */
    dm_rate_memory_t m;
} dm_loss_watch_t;

void dm_loss_watch_arm(dm_loss_watch_t *w, long long now, const dm_rate_memory_t *m);
bool dm_loss_watch_check(dm_loss_watch_t *w, long long now, dm_rate_memory_t *m);
bool dm_loss_watch_loss(dm_loss_watch_t *w, long long now, dm_rate_memory_t *m);

/* The retrains --max-retrains counts. Not every one: a retrain that comes
 * back slower, in either direction, is the rate stepping down to one the
 * line will bear - the cure working - and retrains two minutes apart are a
 * link that works between them, after which the count starts again. */
typedef struct
{
    unsigned seen;            /* the modem's retrain count at the last look */
    unsigned strikes;         /* the ones that count */
    int64_t last_ms;          /* when the latest happened */
    int rates;                /* both directions' rates added, in data, before it */
} dm_retrain_count_t;

/* A look at the modem: its retrain count so far, the rates it runs at each
 * way (tx_rate 0 where the two are one), and whether it is in data. Returns
 * the strikes. */
unsigned dm_retrain_count_update(dm_retrain_count_t *c, unsigned retrains, int rx_rate, int tx_rate,
                                 bool in_data, int64_t now_ms);

/* What the far end sends, handed to the modem as it arrives - in place of a
 * jitter buffer, which plays a fixed delay out against our own clock. That
 * is right for a sound card, which needs a sample every 125 us whatever the
 * network does, and wrong for a modem, which recovers the far end's clock
 * from the signal and can take its samples whenever they come. A fixed
 * buffer only ever grows: a packet late enough to empty it puts a frame of
 * nothing in front of everything after, and a packet more than our clock
 * asked for stays in it to the end of the call. A V.34 call to a BBS
 * through a carrier that added a packet or two every few minutes went from
 * 20 ms of buffer to its 240 ms limit, the round trip from 248 ms to 588,
 * and once it was full each packet more was thrown away, a splice in the
 * signal - the last of them in the middle of a retrain, which ended the
 * call.
 *
 * Packets go to the modem in sequence-number order, as they come. One out of
 * order waits for the one before it up to hold samples; one lost is given up
 * on after hold, and becomes as many samples of nothing as the packet after
 * it holds. With nothing arriving at all, or nothing yet, after hold the
 * modem is given nothing a tick at a time, so its clock runs on as it would
 * through an empty buffer - and that is owed: the packets that come after
 * it, late, are skipped until it is paid off, so a stall leaves no delay
 * behind it, and a path that has become slower for good costs the modem
 * one gap and no more.
 *
 * Not timestamps, though they could place each packet to the sample: the
 * first version of this did, and a carrier's packets toward a calling
 * datamodem - none lost, none out of order - all came to be behind where it
 * expected them, after which it skipped every one, and the modem heard
 * nothing for the rest of the call. The far end's timestamps are counted
 * when they jump, for the log, and otherwise not trusted, as pjmedia's own
 * jitter buffer does not trust them.
 *
 * Lengths are in samples at 8000 a second. One per call; not
 * thread-safe. */
#define DM_RTP_RX_SLOTS 32
#define DM_RTP_RX_MAX_SAMPLES 480       /* 60 ms, the most a packet may hold */
#define DM_RTP_RX_RESYNC 100            /* a jump in sequence numbers this big is a new stream */
#define DM_RTP_RX_MAX_OWED (8000 * 2)   /* the most a stall puts the modem ahead by */

typedef struct
{
    bool used;
    uint16_t seq;
    uint32_t ts;
    int len;
    int16_t s[DM_RTP_RX_MAX_SAMPLES];
} dm_rtp_rx_slot_t;

typedef struct
{
    dm_rtp_rx_slot_t slot[DM_RTP_RX_SLOTS];
    int queued;
    bool started;
    uint32_t ssrc;
    uint16_t next_seq;        /* the packet the modem is due next */
    int waited;               /* samples' worth of ticks with nothing to give */
    int owed;                 /* samples of nothing the modem was given for packets still to come */
    bool have_last;
    uint32_t last_ts;         /* of the last packet given, to see the far end's timestamps jump */
    int last_len;
    /* Counts, for the log. */
    unsigned late;            /* packets skipped: after their place had gone, or to pay off a stall */
    unsigned lost;            /* packets given up on */
    unsigned dup;
    unsigned overflow;        /* packets with no room for them */
    unsigned resyncs;         /* a new SSRC, or sequence numbers that jumped */
    unsigned ts_jumps;        /* timestamps that did not follow on from the packet before */
    unsigned long long filled; /* samples of nothing given the modem */
    int max_queued;           /* the most packets ever waiting */
} dm_rtp_rx_t;

void dm_rtp_rx_reset(dm_rtp_rx_t *q);

/* A packet from the far end: its SSRC, sequence number, timestamp and len
 * samples. Returns false if it was not kept - late, a duplicate, or no
 * room. */
bool dm_rtp_rx_push(dm_rtp_rx_t *q, uint32_t ssrc, uint16_t seq, uint32_t ts, const int16_t *s, int len);

/* A tick of tick samples of our clock: everything that can go to the modem
 * now goes, through out(user, samples, n), with samples NULL for n samples
 * of nothing. */
void dm_rtp_rx_drain(dm_rtp_rx_t *q, int tick, int hold, void (*out)(void *user, const int16_t *s, int n),
                     void *user);

#endif /* DATAMODEM_UTIL_H */
