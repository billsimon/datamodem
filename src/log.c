#include "datamodem/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static dm_log_level_t g_level = DM_LOG_INFO;
static bool g_json = false;
static FILE *g_out = NULL;       /* NULL means stderr, resolved at use */
static bool g_own_out = false;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Writing a line can block - a terminal over a slow link, a pipe nobody is
 * reading, a disk - and lines come from the pjmedia thread too, holding the
 * modem's lock in the middle of a 20 ms frame. A stall there is a gap in the
 * audio. So a line from any thread but the main one is formatted and queued,
 * and a logger thread writes it out; one from the main thread, which waits
 * on the terminal anyway, is written at once - after whatever is queued, so
 * nothing comes out of order. If the queue ever fills, lines are dropped and
 * counted rather than waited for.
 *
 * g_io serialises writing to the sink, and is always taken before g_lock,
 * which guards the queue and the settings. */
#define LOG_QUEUE_BYTES (1024 * 1024)
#define LOG_LINE_MAX 8192

static pthread_mutex_t g_io = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queued = PTHREAD_COND_INITIALIZER;
static unsigned char g_queue[LOG_QUEUE_BYTES];
static size_t g_q_head;
static size_t g_q_len;
static unsigned long g_q_dropped;
static pthread_t g_main;
static bool g_have_main = false;
static pthread_t g_writer;
static bool g_writer_running = false;
static bool g_writer_stop = false;

static const char *const LEVEL_NAMES[] = {"error", "warn", "info", "debug", "trace"};

static FILE *sink(void)
{
    return g_out != NULL ? g_out : stderr;
}

void dm_log_init(dm_log_level_t level, bool json)
{
    pthread_mutex_lock(&g_lock);
    if (!g_have_main)
    {
        g_main = pthread_self();
        g_have_main = true;
    }
    g_level = level;
    g_json = json;
    /* Line buffering keeps ordering sane when the log is a pipe or a file
     * rather than the terminal. */
    setvbuf(sink(), NULL, _IOLBF, 0);
    pthread_mutex_unlock(&g_lock);
}

bool dm_log_set_file(const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "ae");

    if (f == NULL)
    {
        snprintf(err, err_len, "cannot open log file '%s': %s", path, strerror(errno));
        return false;
    }
    setvbuf(f, NULL, _IOFBF, 0);

    pthread_mutex_lock(&g_io);
    pthread_mutex_lock(&g_lock);
    if (g_own_out && g_out != NULL)
        fclose(g_out);
    g_out = f;
    g_own_out = true;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_io);
    return true;
}

/* ------------------------------------------------------------ the queue */

/* Callers hold g_lock. */
static void q_copy_in(const void *src, size_t n)
{
    size_t tail = (g_q_head + g_q_len) % LOG_QUEUE_BYTES;
    size_t first = LOG_QUEUE_BYTES - tail;

    if (first > n)
        first = n;
    memcpy(g_queue + tail, src, first);
    memcpy(g_queue, (const unsigned char *) src + first, n - first);
    g_q_len += n;
}

static void q_copy_out(void *dst, size_t n)
{
    size_t first = LOG_QUEUE_BYTES - g_q_head;

    if (first > n)
        first = n;
    memcpy(dst, g_queue + g_q_head, first);
    memcpy((unsigned char *) dst + first, g_queue, n - first);
    g_q_head = (g_q_head + n) % LOG_QUEUE_BYTES;
    g_q_len -= n;
}

/* A record is its length, then the line. Callers hold g_lock. */
static bool q_push(const char *line, size_t len)
{
    uint32_t n = (uint32_t) len;

    if (g_q_len + sizeof(n) + len > LOG_QUEUE_BYTES)
    {
        g_q_dropped++;
        return false;
    }
    q_copy_in(&n, sizeof(n));
    q_copy_in(line, len);
    return true;
}

static size_t q_pop(char *out)
{
    uint32_t n;

    if (g_q_len == 0)
        return 0;
    q_copy_out(&n, sizeof(n));
    q_copy_out(out, n);
    return n;
}

/* Everything queued, written out. Callers hold g_io, and not g_lock. */
static void drain_locked_io(FILE *f)
{
    static char line[LOG_LINE_MAX + 64];
    unsigned long dropped;

    for (;;)
    {
        size_t n;

        pthread_mutex_lock(&g_lock);
        n = q_pop(line);
        dropped = g_q_dropped;
        g_q_dropped = 0;
        pthread_mutex_unlock(&g_lock);
        if (dropped > 0)
            fprintf(f, "\r(%lu log lines dropped: the log could not keep up)\n", dropped);
        if (n == 0)
            break;
        fwrite(line, 1, n, f);
    }
}

static void *writer_main(void *arg)
{
    (void) arg;
    for (;;)
    {
        bool stop;

        pthread_mutex_lock(&g_lock);
        while (g_q_len == 0 && g_q_dropped == 0 && !g_writer_stop)
            pthread_cond_wait(&g_queued, &g_lock);
        stop = g_writer_stop && g_q_len == 0;
        pthread_mutex_unlock(&g_lock);
        if (stop)
            break;

        pthread_mutex_lock(&g_io);
        drain_locked_io(sink());
        fflush(sink());
        pthread_mutex_unlock(&g_io);
    }
    return NULL;
}

/* Everything queued so far, written out now: at exit, and before anything
 * that is about to make the log's last words matter. */
static void log_flush(void)
{
    pthread_mutex_lock(&g_io);
    drain_locked_io(sink());
    fflush(sink());
    pthread_mutex_unlock(&g_io);
}

void dm_log_close(void)
{
    bool join;

    pthread_mutex_lock(&g_lock);
    join = g_writer_running;
    g_writer_stop = true;
    pthread_cond_signal(&g_queued);
    pthread_mutex_unlock(&g_lock);
    if (join)
        pthread_join(g_writer, NULL);

    pthread_mutex_lock(&g_io);
    drain_locked_io(sink());
    pthread_mutex_lock(&g_lock);
    g_writer_running = false;
    g_writer_stop = false;
    if (g_own_out && g_out != NULL)
    {
        fflush(g_out);
        fclose(g_out);
    }
    else
    {
        fflush(sink());
    }
    g_out = NULL;
    g_own_out = false;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_io);
}

/* The session prints CONNECT/OK/NO CARRIER on stdout. When the diagnostics
 * are going to the same screen they will interleave with the remote system's
 * output, which is worth one warning and worth knowing about in the session
 * code. */
bool dm_log_shares_terminal(void)
{
    return !g_own_out && isatty(STDERR_FILENO) && isatty(STDOUT_FILENO);
}

bool dm_log_level_parse(const char *name, dm_log_level_t *out)
{
    if (name == NULL)
        return false;
    for (size_t i = 0; i < sizeof(LEVEL_NAMES) / sizeof(LEVEL_NAMES[0]); i++)
    {
        if (strcasecmp(name, LEVEL_NAMES[i]) == 0)
        {
            *out = (dm_log_level_t) i;
            return true;
        }
    }
    if (strcasecmp(name, "warning") == 0)
    {
        *out = DM_LOG_WARN;
        return true;
    }
    return false;
}

const char *dm_log_level_name(dm_log_level_t level)
{
    if (level < DM_LOG_ERROR || level > DM_LOG_TRACE)
        return "info";
    return LEVEL_NAMES[level];
}

dm_log_level_t dm_log_get_level(void)
{
    return g_level;
}

bool dm_log_enabled(dm_log_level_t level)
{
    return level <= g_level;
}

static void timestamp(char *buf, size_t len)
{
    struct timeval tv;
    struct tm tm;
    char base[32];

    gettimeofday(&tv, NULL);
    gmtime_r(&tv.tv_sec, &tm);
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf, len, "%s.%03dZ", base, (int) (tv.tv_usec / 1000));
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 7 < out_len; i++)
    {
        unsigned char c = (unsigned char) in[i];
        switch (c)
        {
        case '"':
        case '\\':
            out[o++] = '\\';
            out[o++] = (char) c;
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            out[o++] = '\\';
            out[o++] = 'r';
            break;
        case '\t':
            out[o++] = '\\';
            out[o++] = 't';
            break;
        default:
            if (c < 0x20)
                o += (size_t) snprintf(out + o, out_len - o, "\\u%04x", c);
            else
                out[o++] = (char) c;
            break;
        }
    }
    out[o] = '\0';
}

/* Trailing whitespace and newlines come in from pjsip and spandsp; strip them
 * so a log line stays a log line. */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static void emit(dm_log_level_t level, const char *component, const char *event, const char *msg)
{
    char ts[40];
    /* Per thread rather than on the stack, which in pjsip's threads is not
     * ours to size. */
    static __thread char line[LOG_LINE_MAX];
    int n;

    if (!dm_log_enabled(level))
        return;

    timestamp(ts, sizeof(ts));

    if (g_json)
    {
        char emsg[4096];
        char ecomp[128];
        char eevent[128];
        json_escape(msg, emsg, sizeof(emsg));
        json_escape(component ? component : "datamodem", ecomp, sizeof(ecomp));
        if (event != NULL)
        {
            json_escape(event, eevent, sizeof(eevent));
            n = snprintf(line, sizeof(line),
                         "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}\n",
                         ts, dm_log_level_name(level), ecomp, eevent, emsg);
        }
        else
        {
            n = snprintf(line, sizeof(line), "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"msg\":\"%s\"}\n",
                         ts, dm_log_level_name(level), ecomp, emsg);
        }
    }
    else
    {
        /* A leading CR because the terminal may be in raw mode with a partial
         * line of remote output on it; without it the log line starts wherever
         * the cursor happened to be. Harmless in a file. */
        n = snprintf(line, sizeof(line), "\r%s %-5s [%s] %s\n", ts, dm_log_level_name(level),
                     component ? component : "datamodem", msg);
    }
    if (n < 0)
        return;
    if ((size_t) n >= sizeof(line))
    {
        n = (int) sizeof(line) - 1;
        line[n - 1] = '\n';
    }

    if (!g_have_main || pthread_equal(pthread_self(), g_main))
    {
        FILE *f;

        pthread_mutex_lock(&g_io);
        f = sink();
        drain_locked_io(f);
        fwrite(line, 1, (size_t) n, f);
        fflush(f);
        pthread_mutex_unlock(&g_io);
        return;
    }

    pthread_mutex_lock(&g_lock);
    if (!g_writer_running && !g_writer_stop)
    {
        if (pthread_create(&g_writer, NULL, writer_main, NULL) == 0)
        {
            g_writer_running = true;
            atexit(log_flush);
        }
    }
    if (g_writer_running)
    {
        q_push(line, (size_t) n);
        pthread_cond_signal(&g_queued);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);

    /* No thread to hand it to: write it ourselves, as before. */
    pthread_mutex_lock(&g_io);
    fwrite(line, 1, (size_t) n, sink());
    fflush(sink());
    pthread_mutex_unlock(&g_io);
}

void dm_logf(dm_log_level_t level, const char *component, const char *fmt, ...)
{
    char msg[4096];
    va_list ap;

    if (!dm_log_enabled(level))
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rtrim(msg);
    emit(level, component, NULL, msg);
}

void dm_log_event(dm_log_level_t level, const char *component, const char *event, const char *fmt, ...)
{
    char detail[3072];
    char msg[4096];
    va_list ap;

    if (!dm_log_enabled(level))
        return;

    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    rtrim(detail);

    if (g_json)
    {
        /* Keep the key=value detail as a single field; downstream log shippers
         * split it far better than we can guess at types here. */
        snprintf(msg, sizeof(msg), "%s", detail);
        emit(level, component, event, msg);
    }
    else
    {
        snprintf(msg, sizeof(msg), "%s %s", event, detail);
        emit(level, component, NULL, msg);
    }
}

void dm_log_pjsip_writer(int level, const char *data, int len)
{
    char buf[4096];
    dm_log_level_t mapped;

    /* pjsip levels: 0 fatal, 1 error, 2 warn, 3 info, 4 debug, 5+ trace. */
    if (level <= 1)
        mapped = DM_LOG_ERROR;
    else if (level == 2)
        mapped = DM_LOG_WARN;
    else if (level == 3)
        mapped = DM_LOG_INFO;
    else if (level == 4)
        mapped = DM_LOG_DEBUG;
    else
        mapped = DM_LOG_TRACE;

    if (!dm_log_enabled(mapped))
        return;

    if (len < 0)
        len = 0;
    if ((size_t) len >= sizeof(buf))
        len = (int) sizeof(buf) - 1;
    memcpy(buf, data, (size_t) len);
    buf[len] = '\0';
    rtrim(buf);
    if (buf[0] == '\0')
        return;
    emit(mapped, "sip.stack", NULL, buf);
}

void dm_log_spandsp_message(int level, const char *text)
{
    char buf[4096];
    dm_log_level_t mapped;

    /* spandsp SPAN_LOG_* severities, see spandsp/logging.h. */
    if (level <= 1)
        mapped = DM_LOG_ERROR;
    else if (level <= 4)
        mapped = DM_LOG_WARN;
    else if (level <= 6)
        mapped = DM_LOG_DEBUG;
    else
        mapped = DM_LOG_TRACE;

    if (!dm_log_enabled(mapped))
        return;

    snprintf(buf, sizeof(buf), "%s", text);
    rtrim(buf);
    if (buf[0] == '\0')
        return;
    emit(mapped, "dsp", NULL, buf);
}

void dm_log_spandsp_error(const char *text)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", text);
    rtrim(buf);
    if (buf[0] == '\0')
        return;
    emit(DM_LOG_ERROR, "dsp", NULL, buf);
}
