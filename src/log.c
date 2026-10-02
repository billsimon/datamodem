#include "datamodem/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static dm_log_level_t g_level = DM_LOG_INFO;
static bool g_json = false;
static FILE *g_out = NULL;       /* NULL means stderr, resolved at use */
static bool g_own_out = false;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const LEVEL_NAMES[] = {"error", "warn", "info", "debug", "trace"};

static FILE *sink(void)
{
    return g_out != NULL ? g_out : stderr;
}

void dm_log_init(dm_log_level_t level, bool json)
{
    pthread_mutex_lock(&g_lock);
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
    setvbuf(f, NULL, _IOLBF, 0);

    pthread_mutex_lock(&g_lock);
    if (g_own_out && g_out != NULL)
        fclose(g_out);
    g_out = f;
    g_own_out = true;
    pthread_mutex_unlock(&g_lock);
    return true;
}

void dm_log_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_own_out && g_out != NULL)
    {
        fflush(g_out);
        fclose(g_out);
    }
    g_out = NULL;
    g_own_out = false;
    pthread_mutex_unlock(&g_lock);
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
    FILE *f;

    if (!dm_log_enabled(level))
        return;

    timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&g_lock);
    f = sink();
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
            fprintf(f, "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}\n",
                    ts, dm_log_level_name(level), ecomp, eevent, emsg);
        }
        else
        {
            fprintf(f, "{\"ts\":\"%s\",\"level\":\"%s\",\"component\":\"%s\",\"msg\":\"%s\"}\n", ts,
                    dm_log_level_name(level), ecomp, emsg);
        }
    }
    else
    {
        /* A leading CR because the terminal may be in raw mode with a partial
         * line of remote output on it; without it the log line starts wherever
         * the cursor happened to be. Harmless in a file. */
        fprintf(f, "\r%s %-5s [%s] %s\n", ts, dm_log_level_name(level),
                component ? component : "datamodem", msg);
    }
    fflush(f);
    pthread_mutex_unlock(&g_lock);
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
