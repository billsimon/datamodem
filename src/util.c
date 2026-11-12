#include "datamodem/util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

/* Simulated time, for the selftest. It runs two modems against each other
 * flat out - two minutes of audio in a few tens of milliseconds - so every
 * deadline in the modem is frozen as far as the wall clock is concerned, and
 * nothing that depends on one can be tested there. The harness advances this
 * in step with the audio it generates; it stays zero everywhere else. */
static int64_t dm_clock_offset_ms = 0;

void dm_clock_advance_ms(int64_t ms)
{
    if (ms > 0)
        dm_clock_offset_ms += ms;
}

int64_t dm_now_real_ms(void)
{
    struct timespec ts;

#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t dm_now_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + dm_clock_offset_ms;
}

bool dm_mkdir_parent(const char *path, char *err, size_t err_len)
{
    char buf[1024];
    char *slash;

    if (path == NULL || *path == '\0')
    {
        snprintf(err, err_len, "empty path");
        return false;
    }
    if (strlen(path) >= sizeof(buf))
    {
        snprintf(err, err_len, "path too long: %s", path);
        return false;
    }
    snprintf(buf, sizeof(buf), "%s", path);

    slash = strrchr(buf, '/');
    if (slash == NULL || slash == buf)
        return true; /* current directory, or the root: nothing to create */
    *slash = '\0';

    for (char *p = buf + 1; *p != '\0'; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        {
            snprintf(err, err_len, "mkdir %s: %s", buf, strerror(errno));
            return false;
        }
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    {
        snprintf(err, err_len, "mkdir %s: %s", buf, strerror(errno));
        return false;
    }
    return true;
}

void dm_format_duration(int64_t ms, char *out, size_t out_len)
{
    long total = (long) (ms / 1000);
    long h = total / 3600;
    long m = (total % 3600) / 60;
    long s = total % 60;

    if (h > 0)
        snprintf(out, out_len, "%ldh%02ldm%02lds", h, m, s);
    else if (m > 0)
        snprintf(out, out_len, "%ldm%02lds", m, s);
    else
        snprintf(out, out_len, "%ld.%01lds", s, (long) ((ms % 1000) / 100));
}

void dm_format_byte(int c, char *out, size_t out_len)
{
    if (c >= 0x20 && c < 0x7f)
        snprintf(out, out_len, "%c", (char) c);
    else
        snprintf(out, out_len, "<%02X>", (unsigned) (c & 0xff));
}

bool dm_parse_key_spec(const char *s, int *out)
{
    long n;
    char *end;

    if (s == NULL || *s == '\0' || strcasecmp(s, "none") == 0 || strcasecmp(s, "off") == 0)
    {
        *out = -1;
        return true;
    }
    /* "^]" and friends: the control character the caret names. */
    if (s[0] == '^' && s[1] != '\0' && s[2] == '\0')
    {
        int c = toupper((unsigned char) s[1]);
        if (c == '?')
        {
            *out = 0x7f;
            return true;
        }
        if (c < '@' || c > '_')
            return false;
        *out = c - '@';
        return true;
    }
    /* A single non-digit character stands for itself, so --escape-char '+'
     * reads the way it is meant to. A lone digit is still a number. */
    if (s[1] == '\0' && !isdigit((unsigned char) s[0]))
    {
        *out = (unsigned char) s[0];
        return true;
    }
    errno = 0;
    n = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || n < 0 || n > 255)
        return false;
    *out = (int) n;
    return true;
}

/* Appends c to out, which has room for out_len bytes including the NUL,
 * unless it is a control character or there is no room left. */
static void party_put(char *out, size_t out_len, size_t *n, unsigned char c)
{
    if (c < 0x20 || c == 0x7f || *n + 1 >= out_len)
        return;
    out[(*n)++] = (char) c;
    out[*n] = '\0';
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

void dm_parse_sip_party(const char *s, char *name, size_t name_len, char *user, size_t user_len)
{
    const char *uri;
    const char *lt;
    size_t n = 0;

    if (name_len > 0)
        name[0] = '\0';
    if (user_len > 0)
        user[0] = '\0';
    if (s == NULL || name_len == 0 || user_len == 0)
        return;

    while (*s == ' ' || *s == '\t')
        s++;

    /* The display name: quoted, with backslash escapes, or a run of tokens
     * up to the '<' that opens the URI. */
    lt = strchr(s, '<');
    if (*s == '"')
    {
        for (s++; *s != '\0' && *s != '"'; s++)
        {
            if (*s == '\\' && s[1] != '\0')
                s++;
            party_put(name, name_len, &n, (unsigned char) *s);
        }
        lt = strchr(s, '<');
    }
    else if (lt != NULL)
    {
        const char *end = lt;

        while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        for (; s < end; s++)
            party_put(name, name_len, &n, (unsigned char) *s);
    }
    uri = (lt != NULL) ? lt + 1 : s;

    /* The user part: after the scheme, up to the host (sip, sips) or the
     * first parameter (tel, which has no host). sip:host alone has no user
     * part at all. */
    {
        bool is_tel = false;
        const char *at;
        const char *end;

        n = 0;
        if (strncasecmp(uri, "sip:", 4) == 0)
            uri += 4;
        else if (strncasecmp(uri, "sips:", 5) == 0)
            uri += 5;
        else if (strncasecmp(uri, "tel:", 4) == 0)
        {
            uri += 4;
            is_tel = true;
        }
        else
            return;

        at = strpbrk(uri, is_tel ? ">;?" : "@>;?");
        if (!is_tel && (at == NULL || *at != '@'))
            return;
        end = (at != NULL) ? at : uri + strlen(uri);
        for (; uri < end; uri++)
        {
            if (*uri == '%' && hex_value(uri[1]) >= 0 && hex_value(uri[2]) >= 0)
            {
                party_put(user, user_len, &n,
                          (unsigned char) (hex_value(uri[1]) * 16 + hex_value(uri[2])));
                uri += 2;
            }
            else
            {
                party_put(user, user_len, &n, (unsigned char) *uri);
            }
        }
    }
}

/* Five minutes, then four times as long each time, up to eighty: long
 * enough that a rate the line will not bear costs a failure an hour or so,
 * short enough that a call which met a bad patch gets its speed back. */
#define RATE_MEMORY_FIRST_S 300.0
#define RATE_MEMORY_MAX_S 4800.0
/* A rate that has run this long has shown it can. */
#define RATE_MEMORY_HELD_S 300.0

double dm_rate_memory_failed(dm_rate_memory_t *m, int rate, long long since, long long now)
{
    double hold = RATE_MEMORY_FIRST_S;

    if (rate <= 0 || now - since > (long long) (RATE_MEMORY_HELD_S * 8000.0))
        return 0.0;
    /* A failure above a limit still in force cannot raise it. */
    if (dm_rate_memory_limit(m, now) == 0 || rate < m->failed)
        m->failed = rate;
    m->count++;
    for (unsigned i = 1; i < m->count && hold < RATE_MEMORY_MAX_S; i++)
        hold *= 4.0;
    if (hold > RATE_MEMORY_MAX_S)
        hold = RATE_MEMORY_MAX_S;
    m->until = now + (long long) (hold * 8000.0);
    return hold;
}

int dm_rate_memory_limit(const dm_rate_memory_t *m, long long now)
{
    return (m->failed > 0 && now < m->until) ? m->failed : 0;
}

void dm_rate_memory_text(double hold_s, char *out, size_t out_len)
{
    if (hold_s <= 0.0)
        snprintf(out, out_len, "it ran for five minutes first, so not holding it against the rate");
    else
        snprintf(out, out_len, "staying below it for %.0f minutes", hold_s / 60.0);
}

void dm_path_watch_arm(dm_path_watch_t *w, double rtd_ms, const dm_rate_memory_t *a, const dm_rate_memory_t *b)
{
    w->armed = rtd_ms >= 0.0;
    w->rtd_ms = rtd_ms;
    w->a = *a;
    if (b != NULL)
        w->b = *b;
}

static bool memory_same(const dm_rate_memory_t *x, const dm_rate_memory_t *y)
{
    return x->failed == y->failed && x->until == y->until && x->count == y->count;
}

bool dm_path_watch_judge(dm_path_watch_t *w, double rtd_ms, dm_rate_memory_t *a, dm_rate_memory_t *b)
{
    double moved = rtd_ms - w->rtd_ms;
    bool changed;

    if (!w->armed)
        return false;
    w->armed = false;
    if (moved < 0.0)
        moved = -moved;
    if (rtd_ms < 0.0 || moved < DM_PATH_MOVED_MS)
        return false;
    changed = !memory_same(a, &w->a) || (b != NULL && !memory_same(b, &w->b));
    *a = w->a;
    if (b != NULL)
        *b = w->b;
    return changed;
}

double dm_robust_mean(const double *blocks, int n)
{
    double sorted[64];
    double median, sum = 0.0;
    int used = 0;

    if (n <= 0)
        return 0.0;
    if (n > 64)
        n = 64;
    for (int i = 0; i < n; i++)
    {
        int j = i;

        while (j > 0 && sorted[j - 1] > blocks[i])
        {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = blocks[i];
    }
    median = (n % 2) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    for (int i = 0; i < n; i++)
        if (blocks[i] <= 4.0 * median)
        {
            sum += blocks[i];
            used++;
        }
    return sum / used;
}

void dm_loss_watch_arm(dm_loss_watch_t *w, long long now, const dm_rate_memory_t *m)
{
    w->armed = true;
    w->retrain_at = now;
    w->m = *m;
}

static bool loss_restore(dm_loss_watch_t *w, dm_rate_memory_t *m)
{
    bool changed = !memory_same(m, &w->m);

    *m = w->m;
    w->armed = false;
    return changed;
}

bool dm_loss_watch_check(dm_loss_watch_t *w, long long now, dm_rate_memory_t *m)
{
    if (!w->armed || w->loss_at <= 0 || now - w->loss_at > DM_LOSS_BEFORE_S * 8000LL)
        return false;
    return loss_restore(w, m);
}

bool dm_loss_watch_loss(dm_loss_watch_t *w, long long now, dm_rate_memory_t *m)
{
    w->loss_at = now;
    if (!w->armed)
        return false;
    if (now - w->retrain_at > DM_LOSS_AFTER_S * 8000LL)
    {
        w->armed = false;
        return false;
    }
    return loss_restore(w, m);
}

/* Two quiet minutes and --max-retrains counts afresh. What it is for is a
 * link that never settles - the V.22bis pair that disagree about the rate
 * retrain every few seconds - and a link that has carried data for two
 * minutes since its last retrain is working. Five used to be the figure, and
 * a V.32 bis call that retrained cleanly every three or four minutes through
 * its line's dropouts was one retrain from being hung up. */
#define RETRAIN_QUIET_MS (2 * 60 * 1000)

unsigned dm_retrain_count_update(dm_retrain_count_t *c, unsigned retrains, int rx_rate, int tx_rate,
                                 bool in_data, int64_t now_ms)
{
    int rates = rx_rate + (tx_rate > 0 ? tx_rate : rx_rate);

    if (retrains > c->seen)
    {
        if (c->last_ms != 0 && now_ms - c->last_ms > RETRAIN_QUIET_MS)
            c->strikes = 0;
        if (c->rates == 0 || rates >= c->rates)
            c->strikes += retrains - c->seen;
        c->seen = retrains;
        c->last_ms = now_ms;
    }
    if (in_data && rx_rate > 0)
        c->rates = rates;
    return c->strikes;
}

/* ------------------------------------------------------------ RTP arrival */

static void rtp_rx_restart(dm_rtp_rx_t *q)
{
    for (int i = 0; i < DM_RTP_RX_SLOTS; i++)
        q->slot[i].used = false;
    q->queued = 0;
    q->started = false;
    q->waited = 0;
}

void dm_rtp_rx_reset(dm_rtp_rx_t *q)
{
    memset(q, 0, sizeof(*q));
}

bool dm_rtp_rx_push(dm_rtp_rx_t *q, uint32_t ssrc, uint32_t ts, const int16_t *s, int len)
{
    int free_slot = -1;
    int32_t d;

    if (len <= 0 || len > DM_RTP_RX_MAX_SAMPLES)
    {
        q->overflow++;
        return false;
    }
    if (q->started && ssrc != q->ssrc)
    {
        rtp_rx_restart(q);
        q->resyncs++;
    }
    d = q->started ? (int32_t) (ts - q->next_ts) : 0;
    if (d < -DM_RTP_RX_RESYNC || d > DM_RTP_RX_RESYNC)
    {
        rtp_rx_restart(q);
        q->resyncs++;
    }
    if (!q->started)
    {
        q->started = true;
        q->ssrc = ssrc;
        q->next_ts = ts;
        d = 0;
    }
    /* Its place has gone by: the modem was given nothing for it. */
    if (d + len <= 0)
    {
        q->late++;
        return false;
    }
    for (int i = 0; i < DM_RTP_RX_SLOTS; i++)
    {
        if (q->slot[i].used && q->slot[i].ts == ts)
        {
            q->dup++;
            return false;
        }
        if (!q->slot[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
    {
        q->overflow++;
        return false;
    }
    q->slot[free_slot].used = true;
    q->slot[free_slot].ts = ts;
    q->slot[free_slot].len = len;
    memcpy(q->slot[free_slot].s, s, (size_t) len * sizeof(int16_t));
    q->queued++;
    if (q->queued > q->max_queued)
        q->max_queued = q->queued;
    return true;
}

/* The packet waiting that comes first, or -1. */
static int rtp_rx_head(const dm_rtp_rx_t *q)
{
    int best = -1;
    int32_t best_d = 0;

    for (int i = 0; i < DM_RTP_RX_SLOTS; i++)
    {
        int32_t d;

        if (!q->slot[i].used)
            continue;
        d = (int32_t) (q->slot[i].ts - q->next_ts);
        if (best < 0 || d < best_d)
        {
            best = i;
            best_d = d;
        }
    }
    return best;
}

void dm_rtp_rx_drain(dm_rtp_rx_t *q, int tick, int hold, void (*out)(void *user, const int16_t *s, int n),
                     void *user)
{
    bool gave = false;
    int i;

    while ((i = rtp_rx_head(q)) >= 0)
    {
        dm_rtp_rx_slot_t *p = &q->slot[i];
        int32_t d = (int32_t) (p->ts - q->next_ts);

        if (d > 0)
        {
            /* Something missing before it: wait for that, up to hold, then
             * give the modem nothing in its place. */
            if (q->waited < hold)
                break;
            out(user, NULL, (int) d);
            q->filled += (unsigned long long) d;
            q->next_ts = p->ts;
            d = 0;
        }
        /* d <= 0: all of it, or what is left of it after a wait gave up on
         * the start. */
        if (d + p->len > 0)
        {
            out(user, p->s - d, p->len + (int) d);
            q->next_ts = p->ts + (uint32_t) p->len;
        }
        else
        {
            q->late++;
        }
        p->used = false;
        q->queued--;
        gave = true;
    }
    if (gave)
    {
        q->waited = 0;
        return;
    }
    q->waited += tick;
    /* Nothing at all for longer than hold - or nothing yet: the modem's clock
     * runs on, as it would through a gap on a line. */
    if (q->queued == 0 && q->waited > hold)
    {
        out(user, NULL, tick);
        if (q->started)
        {
            q->filled += (unsigned long long) tick;
            q->next_ts += (uint32_t) tick;
        }
    }
}
