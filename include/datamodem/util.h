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

#endif /* DATAMODEM_UTIL_H */
