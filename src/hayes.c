#include "datamodem/hayes.h"
#include "datamodem/log.h"
#include "datamodem/modem.h"
#include "datamodem/session.h"
#include "datamodem/sip.h"
#include "datamodem/tty.h"
#include "datamodem/version.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define HAYES_POLL_MS 100

/* Hayes allowed 40 characters after the AT; init strings written for later
 * modems run longer, and a dial string can be a SIP URI. */
#define HAYES_LINE_MAX 256

typedef enum
{
    WAIT_A,  /* everything up to an A is ignored, as on a real modem */
    WAIT_T,  /* an A: a T starts a command, a slash repeats the last one */
    IN_LINE  /* collecting the command, up to S3 */
} hayes_state_t;

typedef struct
{
    const dm_config_t *cfg;
    volatile sig_atomic_t *stop;
    int fd;              /* the port, which is also our stdin and stdout */
    bool lines;          /* it has modem-control lines; a pty does not */
    bool their_dtr;      /* DSR, the last we looked */
    bool dtr_fell;       /* DSR dropped during the call just ended */
    bool sip_started;

    /* The settings, as AT&F leaves them. */
    bool echo;           /* E */
    bool quiet;          /* Q */
    bool verbose;        /* V */
    int x;               /* X: which result codes there are */
    int dcd_mode;        /* &C: 0 always on, 1 follows the carrier */
    int dtr_mode;        /* &D: 0 ignored, 1 command mode, 2 hang up, 3 and reset */
    bool rtscts;         /* &K3, or &K0 */
    int sreg[256];

    hayes_state_t state;
    char line[HAYES_LINE_MAX];
    size_t len;
    bool overflow;
    char last[HAYES_LINE_MAX];    /* what A/ repeats */
    char last_number[DM_STR_MAX]; /* what ATDL dials */

    /* While ATD waits for an answer: anything typed, or DTR dropping,
     * abandons the call. Set from the watcher thread. */
    volatile sig_atomic_t dialling;
    volatile sig_atomic_t abandon;
    bool abandoned_by_dtr;
} hayes_t;

/* The result-code hook has nowhere to carry a pointer, and there is only
 * ever the one modem. */
static hayes_t *g_hayes;

/* ------------------------------------------------------------- talking */

static void say_framed(const hayes_t *h, const char *text)
{
    char buf[320];
    int n = snprintf(buf, sizeof(buf), "%c%c%s%c%c", h->sreg[3], h->sreg[4], text, h->sreg[3], h->sreg[4]);

    if (n > 0)
        dm_tty_control(buf, (size_t) n < sizeof(buf) ? (size_t) n : sizeof(buf) - 1);
}

/* Information text - ATI, AT&V, ATSn? - framed as a result code is, and
 * said whatever Q is: Q only ever silenced the result codes. */
static void info(const hayes_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void info(const hayes_t *h, const char *fmt, ...)
{
    char text[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    say_framed(h, text);
}

static const struct
{
    const char *word;
    int number;
} RESULT_CODES[] = {
    {"OK", 0},          {"CONNECT", 1}, {"RING", 2},  {"NO CARRIER", 3},
    {"ERROR", 4},       {"NO DIALTONE", 6}, {"BUSY", 7}, {"NO ANSWER", 8},
};

/* Every result code goes through here, the session's included, so that V, Q
 * and X apply to all of them. X0 is the original Hayes set: CONNECT without
 * the rate, and NO CARRIER for anything else that went wrong. X1 adds the
 * rate, X2 NO DIALTONE, X3 BUSY and NO ANSWER without NO DIALTONE, X4 the
 * lot. */
static void say(const char *code)
{
    const hayes_t *h = g_hayes;
    const char *text = code;
    bool connect = (strncmp(code, "CONNECT", 7) == 0);

    if (h == NULL || h->quiet)
        return;
    if (connect && h->x == 0)
        text = "CONNECT";
    else if (strcmp(code, "NO DIALTONE") == 0 && (h->x < 2 || h->x == 3))
        text = "NO CARRIER";
    else if ((strcmp(code, "BUSY") == 0 || strcmp(code, "NO ANSWER") == 0) && h->x < 3)
        text = "NO CARRIER";

    if (!h->verbose)
    {
        for (size_t i = 0; i < sizeof(RESULT_CODES) / sizeof(RESULT_CODES[0]); i++)
        {
            if (connect ? RESULT_CODES[i].number == 1 : strcmp(text, RESULT_CODES[i].word) == 0)
            {
                char buf[8];
                int n = snprintf(buf, sizeof(buf), "%d%c", RESULT_CODES[i].number, h->sreg[3]);

                dm_tty_control(buf, (size_t) n);
                return;
            }
        }
    }
    say_framed(h, text);
}

const char *dm_hayes_dial_failure(int rc, int sip_status)
{
    if (rc == DM_EXIT_SIP)
        return "NO DIALTONE";      /* no registration, no transport: no line */
    if (rc == DM_EXIT_TIMEOUT)
        return "NO ANSWER";
    switch (sip_status)
    {
    case 486: /* Busy Here */
    case 600: /* Busy Everywhere */
    case 603: /* Decline */
        return "BUSY";
    case 408: /* Request Timeout */
    case 480: /* Temporarily Unavailable */
        return "NO ANSWER";
    case 503: /* Service Unavailable: the trunk, not the number */
        return "NO DIALTONE";
    default:
        return "NO CARRIER";
    }
}

/* ------------------------------------------------------------ the port */

static bool port_speed(int rate, speed_t *out)
{
    static const struct
    {
        int rate;
        speed_t speed;
    } speeds[] = {
        {300, B300},       {600, B600},       {1200, B1200},     {2400, B2400},
        {4800, B4800},     {9600, B9600},     {19200, B19200},   {38400, B38400},
        {57600, B57600},   {115200, B115200}, {230400, B230400},
#ifdef B460800
        {460800, B460800},
#endif
#ifdef B921600
        {921600, B921600},
#endif
    };

    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++)
    {
        if (speeds[i].rate == rate)
        {
            *out = speeds[i].speed;
            return true;
        }
    }
    return false;
}

/* Cygwin calls COM4 /dev/ttyS3; let people say what Windows calls it. */
static void port_path(const char *name, char *out, size_t out_len)
{
#ifdef __CYGWIN__
    if (strncasecmp(name, "COM", 3) == 0 && name[3] != '\0' && strspn(name + 3, "0123456789") == strlen(name + 3) &&
        atoi(name + 3) > 0)
    {
        snprintf(out, out_len, "/dev/ttyS%d", atoi(name + 3) - 1);
        return;
    }
#endif
    snprintf(out, out_len, "%s", name);
}

#ifdef __CYGWIN__
static int com_cmp(const void *a, const void *b)
{
    return atoi((const char *) a + 3) - atoi((const char *) b + 3);
}

/* The COM ports Windows has, as "COM8, COM9", from the same registry key
 * every Windows program looks in - com0com's pairs are there too. */
static void windows_ports(char *out, size_t out_len)
{
    static const char key[] = "/proc/registry/HKEY_LOCAL_MACHINE/HARDWARE/DEVICEMAP/SERIALCOMM";
    char names[64][16];
    size_t n = 0;
    struct dirent *e;
    DIR *d;

    out[0] = '\0';
    d = opendir(key);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL && n < 64)
    {
        char file[512];
        FILE *f;
        size_t len;

        if (e->d_name[0] == '.')
            continue;
        snprintf(file, sizeof(file), "%s/%s", key, e->d_name);
        f = fopen(file, "rb");
        if (f == NULL)
            continue;
        len = fread(names[n], 1, sizeof(names[n]) - 1, f);
        fclose(f);
        names[n][len] = '\0';
        names[n][strcspn(names[n], "\r\n")] = '\0';
        if (strncasecmp(names[n], "COM", 3) == 0)
            n++;
    }
    closedir(d);
    qsort(names, n, sizeof(names[0]), com_cmp);
    for (size_t i = 0; i < n; i++)
        snprintf(out + strlen(out), out_len - strlen(out), "%s%s", i ? ", " : "", names[i]);
}
#endif

static void say_cannot_open(const char *name, const char *path, int err)
{
#ifdef __CYGWIN__
    char ports[256];

    windows_ports(ports, sizeof(ports));
    if (strcmp(name, path) != 0)
        DM_ERROR("modem", "cannot open %s (%s): %s", name, path, strerror(err));
    else
        DM_ERROR("modem", "cannot open %s: %s", path, strerror(err));
    if (ports[0] != '\0')
        DM_ERROR("modem", "the serial ports Windows has: %s", ports);
    else
        DM_ERROR("modem", "Windows has no serial ports; for a terminal program, make a com0com "
                          "pair such as COM8 and COM9 (see the README)");
#else
    (void) name;
    DM_ERROR("modem", "cannot open %s: %s", path, strerror(err));
#endif
}

static int open_port(const dm_config_t *cfg, const char *path)
{
    struct termios t;
    speed_t speed;
    int fd;

    if (!port_speed(cfg->port_speed, &speed))
    {
        DM_ERROR("modem", "%d is not a speed a serial port runs at; try 115200", cfg->port_speed);
        return -1;
    }
    /* Non-blocking only to open it: a port waiting on DCD would otherwise
     * hold us here until something raised it. */
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        say_cannot_open(cfg->port, path, errno);
        return -1;
    }
    if (!isatty(fd) || tcgetattr(fd, &t) != 0)
    {
        DM_ERROR("modem", "%s is not a serial port", path);
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    /* Raw, as the session's terminal is: every byte through unaltered. */
    t.c_iflag &= (tcflag_t) ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR | ISTRIP | INPCK | BRKINT | PARMRK);
    t.c_iflag |= IGNBRK;
    t.c_oflag &= (tcflag_t) ~OPOST;
    t.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
    /* CLOCAL: our own DCD is the terminal's DTR, which is ours to watch,
     * not something that should block us or hang up the port. HUPCL drops
     * DTR - the terminal's carrier - when we exit. */
    t.c_cflag &= (tcflag_t) ~(CSIZE | PARENB | CSTOPB);
    t.c_cflag |= CS8 | CREAD | CLOCAL | HUPCL;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    if (tcsetattr(fd, TCSANOW, &t) != 0)
    {
        DM_ERROR("modem", "cannot set up %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void set_dtr(const hayes_t *h, bool on)
{
    int bit = TIOCM_DTR;

    if (h->lines && ioctl(h->fd, on ? TIOCMBIS : TIOCMBIC, &bit) != 0)
        DM_WARN("modem", "cannot %s DTR: %s", on ? "raise" : "drop", strerror(errno));
}

/* The terminal's DTR, which arrives as our DSR. False when there is no
 * telling. */
static bool read_their_dtr(const hayes_t *h, bool *high)
{
    int st;

    if (!h->lines || ioctl(h->fd, TIOCMGET, &st) != 0)
        return false;
    *high = (st & TIOCM_DSR) != 0;
    return true;
}

/* Puts the port's flow control and DCD where the settings say, between
 * calls: &C0 holds DCD up always, &C1 keeps it down until there is a
 * carrier. */
static void apply_lines(hayes_t *h)
{
    struct termios t;

    if (tcgetattr(h->fd, &t) == 0)
    {
        if (h->rtscts)
            t.c_cflag |= CRTSCTS;
        else
            t.c_cflag &= (tcflag_t) ~CRTSCTS;
        tcsetattr(h->fd, TCSANOW, &t);
    }
    set_dtr(h, h->dcd_mode == 0);
}

static void carrier(void *user, bool up)
{
    hayes_t *h = user;

    if (h->dcd_mode == 1)
        set_dtr(h, up);
}

/* True once each time the terminal's DTR goes from up to down. One that was
 * never up - a terminal program that does not raise it - never hangs up. */
static bool dtr_dropped(void *user)
{
    hayes_t *h = user;
    bool high;
    bool dropped;

    if (h->dtr_mode == 0 || !read_their_dtr(h, &high))
        return false;
    dropped = h->their_dtr && !high;
    h->their_dtr = high;
    if (dropped)
        h->dtr_fell = true;
    return dropped;
}

/* ------------------------------------------------------------ settings */

static void factory(hayes_t *h)
{
    h->echo = true;
    h->quiet = false;
    h->verbose = true;
    h->x = 4;
    h->dcd_mode = 1;
    h->dtr_mode = 2;
    h->rtscts = (strcasecmp(h->cfg->port_flow, "rtscts") == 0);
    memset(h->sreg, 0, sizeof(h->sreg));
    h->sreg[2] = h->cfg->escape_char;
    h->sreg[3] = '\r';
    h->sreg[4] = '\n';
    h->sreg[5] = '\b';
    h->sreg[6] = 2;
    h->sreg[7] = 50;
    h->sreg[8] = 2;
    h->sreg[10] = 14;
    /* In milliseconds, as --escape-guard-ms and the ATS12 of a call in
     * progress are, rather than a real modem's fiftieths of a second. */
    h->sreg[12] = h->cfg->escape_guard_ms;
}

static bool set_sreg(hayes_t *h, int reg, int value)
{
    if (reg == 12)
    {
        if (value < 100 || value > 10000)
            return false;
    }
    else if (value < 0 || value > 255)
    {
        return false;
    }
    h->sreg[reg] = value;
    return true;
}

static void print_settings(const hayes_t *h)
{
    info(h, "E%d Q%d V%d X%d &C%d &D%d &K%d", h->echo, h->quiet, h->verbose, h->x, h->dcd_mode, h->dtr_mode,
         h->rtscts ? 3 : 0);
    info(h, "S00:%03d S02:%03d S03:%03d S04:%03d S05:%03d S07:%03d S12:%d", h->sreg[0], h->sreg[2], h->sreg[3],
         h->sreg[4], h->sreg[5], h->sreg[7], h->sreg[12]);
    info(h, "port %s at %d bps, %s", h->cfg->port, h->cfg->port_speed,
         h->lines ? "DCD follows &C, DTR is read as DSR" : "no control lines");
}

/* ------------------------------------------------------------- calling */

static void *dial_watch(void *arg)
{
    hayes_t *h = arg;

    while (h->dialling)
    {
        struct pollfd p = {h->fd, POLLIN, 0};

        if (*h->stop != 0)
        {
            h->abandon = 1;
            break;
        }
        if (h->dtr_mode != 0 && dtr_dropped(h))
        {
            h->abandoned_by_dtr = true;
            h->abandon = 1;
            break;
        }
        if (poll(&p, 1, 50) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)))
        {
            unsigned char c;
            ssize_t n = read(h->fd, &c, 1);

            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            /* A keystroke abandons the call, as on any Hayes. So does the
             * port going away. */
            h->abandon = 1;
            break;
        }
    }
    return NULL;
}

static void dial(hayes_t *h, const char *number)
{
    dm_config_t cfg = *h->cfg;
    dm_modem_params_t params;
    dm_modem_t *modem;
    dm_session_result_t sr;
    dm_session_port_t port;
    dm_call_result_t cr;
    pthread_t watcher;
    bool watching;
    int rc;

    snprintf(h->last_number, sizeof(h->last_number), "%s", number);
    cfg.escape_char = h->sreg[2];
    cfg.escape_guard_ms = h->sreg[12];
    /* The port reading end-of-file means it has gone, and with it whoever
     * was calling: there is no reply to wait for. */
    cfg.hangup_on_eof = true;

    if (!h->sip_started)
    {
        rc = dm_sip_start(&cfg);
        if (rc != DM_EXIT_OK)
        {
            dm_sip_stop();
            say(dm_hayes_dial_failure(rc, 0));
            return;
        }
        h->sip_started = true;
    }

    dm_modem_params_from_config(&cfg, true, "out", &params);
    modem = dm_modem_create(&params);
    if (modem == NULL)
    {
        say("ERROR");
        return;
    }

    h->abandon = 0;
    h->abandoned_by_dtr = false;
    h->dtr_fell = false;
    read_their_dtr(h, &h->their_dtr);
    h->dialling = 1;
    watching = (pthread_create(&watcher, NULL, dial_watch, h) == 0);
    rc = dm_sip_dial(&cfg, number, modem, watching ? &h->abandon : h->stop);
    h->dialling = 0;
    if (watching)
        pthread_join(watcher, NULL);

    if (rc != DM_EXIT_OK)
    {
        dm_sip_hangup(&cr);
        dm_modem_destroy(modem);
        if (h->abandoned_by_dtr)
            DM_INFO("modem", "the terminal dropped DTR; the call is abandoned");
        else if (h->abandon)
            say("NO CARRIER");
        else
            say(dm_hayes_dial_failure(rc, cr.sip_status));
        return;
    }

    port.carrier = carrier;
    port.dtr_dropped = dtr_dropped;
    port.dtr_escapes = (h->dtr_mode == 1);
    port.user = h;
    dm_session_run(&cfg, modem, h->stop, &port, &sr);
    carrier(h, false);
    dm_sip_hangup(&cr);
    dm_modem_destroy(modem);
    dm_log_event(DM_LOG_INFO, "call", "cleared", "remote=\"%s\" sip_status=%d sip_reason=\"%s\" duration_ms=%d",
                 cr.remote_uri, cr.sip_status, cr.sip_reason, cr.duration_ms);

    if (h->dtr_fell && h->dtr_mode == 3)
    {
        factory(h);
        apply_lines(h);
    }
}

/* ATD, and whatever follows it on the line: the number, the way a dialling
 * directory has it - "1 (555) 123-4567", "9W5551234" - or a SIP URI as it
 * stands. What made sense to a telephone exchange - tone or pulse, wait for
 * dial tone, pause, flash - means nothing to a SIP trunk and is dropped. */
static void dial_command(hayes_t *h, const char *s)
{
    char number[DM_STR_MAX];
    size_t n = 0;

    while (*s == 'T' || *s == 't' || *s == 'P' || *s == 'p')
        s++;

    if ((*s == 'L' || *s == 'l') && s[1] == '\0')
    {
        if (h->last_number[0] == '\0')
        {
            say("ERROR");
            return;
        }
        snprintf(number, sizeof(number), "%s", h->last_number);
    }
    else if (strncasecmp(s, "sip:", 4) == 0 || strncasecmp(s, "sips:", 5) == 0)
    {
        snprintf(number, sizeof(number), "%s", s);
        n = strlen(number);
        while (n > 0 && number[n - 1] == ';')
            number[--n] = '\0';
    }
    else if ((*s == 'S' || *s == 's') && (s[1] == '\0' || s[1] == '=' || isdigit((unsigned char) s[1])))
    {
        say("ERROR"); /* ATDS: there are no stored numbers */
        return;
    }
    else
    {
        for (; *s != '\0' && n + 1 < sizeof(number); s++)
        {
            if (strchr("TtPpWw,;@!-()./", *s) != NULL)
                continue;
            if (isalnum((unsigned char) *s) || *s == '+' || *s == '*' || *s == '#')
                number[n++] = *s;
        }
        number[n] = '\0';
    }
    if (number[0] == '\0')
    {
        say("ERROR");
        return;
    }
    dial(h, number);
}

/* -------------------------------------------------------- the commands */

/* The number after a command letter, or none when there is not one. */
static int arg(const char **p, int none)
{
    int v = 0;
    bool any = false;

    while (isdigit((unsigned char) **p))
    {
        if (v < 100000)
            v = v * 10 + (**p - '0');
        (*p)++;
        any = true;
    }
    return any ? v : none;
}

/* An extended command - +MS=..., or a value given to &Z - runs to the next
 * semicolon. */
static void skip_value(const char **p)
{
    while (**p != '\0' && **p != ';')
        (*p)++;
    if (**p == ';')
        (*p)++;
}

static bool ampersand(hayes_t *h, const char **p)
{
    int c = toupper((unsigned char) **p);
    int v;

    if (c == '\0')
        return false;
    (*p)++;
    v = arg(p, 0);
    switch (c)
    {
    case 'C':
        if (v > 1)
            return false;
        h->dcd_mode = v;
        apply_lines(h);
        return true;
    case 'D':
        if (v > 3)
            return false;
        h->dtr_mode = v;
        return true;
    case 'F':
        factory(h);
        apply_lines(h);
        return true;
    case 'K':
        /* &K4 and up are XON/XOFF, which a terminal program can do for
         * itself; they are taken and change nothing. */
        if (v > 6)
            return false;
        if (v == 0 || v == 3)
        {
            h->rtscts = (v == 3);
            apply_lines(h);
        }
        return true;
    case 'V':
        print_settings(h);
        return true;
    default:
        /* &W, &Y, &S, &Q and the rest. There is nothing to store and no
         * other dialect to switch to, so a modem with none says OK: an init
         * string written for some other modem still works. */
        if (**p == '=')
            skip_value(p);
        return true;
    }
}

/* Runs one command line, everything after the AT. Returns true when it
 * placed a call, whatever came of it. */
static bool execute(hayes_t *h, const char *line)
{
    char cmd[HAYES_LINE_MAX];
    const char *p = cmd;
    size_t n = 0;

    /* Spaces mean nothing anywhere in a command, the dial string included. */
    for (const char *r = line; *r != '\0' && n + 1 < sizeof(cmd); r++)
        if (*r != ' ' && *r != '\t')
            cmd[n++] = *r;
    cmd[n] = '\0';

    while (*p != '\0')
    {
        int c = toupper((unsigned char) *p++);
        int v;

        switch (c)
        {
        case 'D':
            dial_command(h, p);
            return true;
        case 'E':
        case 'Q':
        case 'V':
            if ((v = arg(&p, 0)) > 1)
                goto error;
            if (c == 'E')
                h->echo = v;
            else if (c == 'Q')
                h->quiet = v;
            else
                h->verbose = v;
            break;
        case 'X':
            if ((v = arg(&p, 0)) > 7)
                goto error;
            h->x = v > 4 ? 4 : v;
            break;
        case 'H':
            if (arg(&p, 0) > 1)
                goto error;
            break;
        case 'Z':
            arg(&p, 0);
            factory(h);
            apply_lines(h);
            break;
        case 'O':
            arg(&p, 0);
            say("NO CARRIER"); /* there is no call to go back to */
            return false;
        case 'I':
            arg(&p, 0);
            info(h, "datamodem %s", DATAMODEM_VERSION);
            break;
        case 'A':
            /* Answering is `datamodem answer`'s job. */
            goto error;
        case 'S':
        {
            int reg = arg(&p, -1);

            if (reg < 0 || reg > 255)
                goto error;
            if (*p == '?')
            {
                p++;
                info(h, "%03d", h->sreg[reg]);
            }
            else if (*p == '=')
            {
                p++;
                if (!set_sreg(h, reg, arg(&p, -1)))
                    goto error;
            }
            else
            {
                goto error;
            }
            break;
        }
        case '&':
            if (!ampersand(h, &p))
                goto error;
            break;
        case '\\':
        case '%':
            /* \N3, %C1: error correction and compression in some vendor's
             * dialect. Ours are --v42 and --v42bis. */
            if (*p != '\0')
                p++;
            arg(&p, 0);
            break;
        case '+':
        case '*':
        case '#':
        case '$':
            skip_value(&p);
            break;
        case 'B': /* Bell or CCITT at 300 and 1200: stepping down asks the far end */
        case 'C':
        case 'F':
        case 'L': /* speaker volume */
        case 'M': /* speaker: --speaker */
        case 'N':
        case 'P':
        case 'T':
        case 'W':
        case 'Y':
            arg(&p, 0);
            break;
        default:
            goto error;
        }
    }
    say("OK");
    return false;

error:
    say("ERROR");
    return false;
}

/* One byte from the terminal while there is no call. Returns true when it
 * finished a command that placed a call; whatever else was typed alongside
 * it is the terminal's typing-ahead, which a real modem would not have kept
 * either. */
static bool command_byte(hayes_t *h, unsigned char c)
{
    /* A terminal set to 7E1 still types AT; its parity bit is not part of
     * the command. */
    unsigned char ch = c & 0x7f;

    if (h->echo)
        dm_tty_control(&c, 1);

    switch (h->state)
    {
    case WAIT_A:
        if (ch == 'A' || ch == 'a')
            h->state = WAIT_T;
        break;
    case WAIT_T:
        if (ch == 'T' || ch == 't')
        {
            h->state = IN_LINE;
            h->len = 0;
            h->overflow = false;
        }
        else if (ch == '/')
        {
            h->state = WAIT_A;
            return execute(h, h->last);
        }
        else if (ch != 'A' && ch != 'a')
        {
            h->state = WAIT_A;
        }
        break;
    case IN_LINE:
        if (ch == h->sreg[3])
        {
            h->line[h->len] = '\0';
            h->state = WAIT_A;
            if (h->overflow)
            {
                say("ERROR");
                break;
            }
            snprintf(h->last, sizeof(h->last), "%s", h->line);
            return execute(h, h->line);
        }
        if (ch == h->sreg[5] || ch == 0x7f)
        {
            if (h->len > 0)
                h->len--;
            if (h->echo)
                dm_tty_control(" \b", 2);
        }
        else if (ch >= 0x20)
        {
            if (h->len + 1 < sizeof(h->line))
                h->line[h->len++] = (char) ch;
            else
                h->overflow = true;
        }
        break;
    }
    return false;
}

/* ---------------------------------------------------------------- run */

int dm_hayes_run(const dm_config_t *cfg, volatile sig_atomic_t *stop)
{
    static hayes_t h;
    char path[DM_STR_MAX];
    int st;
    int fd;
    int rc = DM_EXIT_OK;

    memset(&h, 0, sizeof(h));
    h.cfg = cfg;
    h.stop = stop;
    port_path(cfg->port, path, sizeof(path));
    fd = open_port(cfg, path);
    if (fd < 0)
        return DM_EXIT_CONFIG;

    /* From here the port is our stdin and stdout, so the session - which
     * talks to stdin and stdout - needs to know nothing about it. The log
     * stays on stderr. */
    if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0)
    {
        DM_ERROR("modem", "dup2: %s", strerror(errno));
        close(fd);
        return DM_EXIT_INTERNAL;
    }
    close(fd);
    h.fd = STDIN_FILENO;
    h.lines = (ioctl(h.fd, TIOCMGET, &st) == 0);
    g_hayes = &h;
    dm_tty_set_line(false);
    dm_tty_set_result_hook(say);

    factory(&h);
    apply_lines(&h);
    if (h.lines)
        DM_INFO("modem", "a modem on %s at %d bps: ATD dials; DCD is our DTR, and the terminal's DTR "
                         "is our DSR",
                path, cfg->port_speed);
    else
        DM_INFO("modem", "a modem on %s at %d bps: ATD dials. It has no modem-control lines, so DCD "
                         "and DTR mean nothing here; +++ and ATH hang up",
                path, cfg->port_speed);

    /* Up now, rather than at the first ATD, so that a registration has
     * already happened by the time anyone dials. A failure is not the end:
     * the next ATD tries again, and says NO DIALTONE if that fails too. */
    if (dm_sip_start(cfg) == DM_EXIT_OK)
        h.sip_started = true;
    else
        dm_sip_stop();

    while (*stop == 0)
    {
        struct pollfd p = {h.fd, POLLIN, 0};
        unsigned char buf[256];
        ssize_t n;

        if (poll(&p, 1, HAYES_POLL_MS) <= 0)
            continue;
        n = read(h.fd, buf, sizeof(buf));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
        {
            DM_ERROR("modem", "%s has gone away%s%s", path, n < 0 ? ": " : "", n < 0 ? strerror(errno) : "");
            rc = DM_EXIT_CONFIG;
            break;
        }
        for (ssize_t i = 0; i < n; i++)
            if (command_byte(&h, buf[i]))
                break;
    }

    set_dtr(&h, false);
    dm_tty_set_result_hook(NULL);
    g_hayes = NULL;
    if (h.sip_started)
        dm_sip_stop();
    return rc;
}
