#include "datamodem/session.h"
#include "datamodem/log.h"
#include "datamodem/sip.h"
#include "datamodem/tty.h"
#include "datamodem/util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fine enough for the escape guard timer and for the watchdogs, coarse
 * enough that an idle call costs nothing. */
#define DM_POLL_MS 100

/* How long a non-interactive run keeps listening after its input has ended
 * and the transmit queue has drained. A script pipes a command in and wants
 * the answer; without this the call would clear before the far end replied.
 * Overridden by --idle-timeout when that is set. */
#define DM_EOF_IDLE_S 30

#define DM_READ_CHUNK 1024

/* ------------------------------------------------------- escape detector */

void dm_escape_init(dm_escape_t *e, int escape_char, int guard_ms)
{
    memset(e, 0, sizeof(*e));
    e->escape_char = escape_char;
    e->guard_ms = guard_ms;
    /* The leading guard period starts now, so "+++" typed immediately after
     * CONNECT works the same as "+++" typed an hour in. */
    e->last_data_ms = dm_now_ms();
}

static int release_held(dm_escape_t *e, unsigned char *out)
{
    int n = 0;

    for (int i = 0; i < e->count; i++)
        out[n++] = (unsigned char) e->escape_char;
    e->count = 0;
    return n;
}

int dm_escape_feed(dm_escape_t *e, int64_t now_ms, unsigned char c, unsigned char *out)
{
    int n;

    if (e->escape_char < 0 || e->escape_char > 127 || e->guard_ms <= 0)
    {
        out[0] = c;
        return 1;
    }

    if ((int) c == e->escape_char)
    {
        if (e->count == 0)
        {
            if (now_ms - e->last_data_ms >= e->guard_ms)
            {
                e->count = 1;
                e->last_plus_ms = now_ms;
                return 0; /* held back until we know what this is */
            }
            /* No leading silence, so it is just a plus sign in the data. */
            e->last_data_ms = now_ms;
            out[0] = c;
            return 1;
        }
        if (e->count < 3 && now_ms - e->last_plus_ms < e->guard_ms)
        {
            e->count++;
            e->last_plus_ms = now_ms;
            return 0;
        }
        /* A fourth one, or one that came too late: not the sequence after
         * all. Everything held goes out, in order, followed by this one. */
        n = release_held(e, out);
        out[n++] = c;
        e->last_data_ms = now_ms;
        return n;
    }

    n = release_held(e, out);
    out[n++] = c;
    e->last_data_ms = now_ms;
    return n;
}

bool dm_escape_tick(dm_escape_t *e, int64_t now_ms, unsigned char *out, int *out_len)
{
    *out_len = 0;
    if (e->count == 0)
        return false;

    if (e->count >= 3)
    {
        if (now_ms - e->last_plus_ms < e->guard_ms)
            return false;
        e->count = 0;
        e->tripped = true;
        return true;
    }

    /* One or two, and the window for the next one has closed. */
    if (now_ms - e->last_plus_ms >= e->guard_ms)
    {
        *out_len = release_held(e, out);
        e->last_data_ms = now_ms;
    }
    return false;
}

/* ------------------------------------------------------------- session */

const char *dm_session_end_name(dm_session_end_t reason)
{
    switch (reason)
    {
    case DM_END_HANGUP:
        return "hung up locally";
    case DM_END_NO_CARRIER:
        return "no carrier";
    case DM_END_REMOTE_CLEARED:
        return "the far end cleared the call";
    case DM_END_TRAIN_FAILED:
        return "the modems never trained";
    case DM_END_IDLE:
        return "idle timeout";
    case DM_END_UNSTABLE:
        return "the link would not hold";
    case DM_END_TIMEOUT:
        return "timeout";
    case DM_END_SIGNAL:
        return "interrupted";
    case DM_END_ERROR:
        return "error";
    }
    return "?";
}

typedef struct
{
    const dm_config_t *cfg;
    dm_modem_t *modem;
    volatile sig_atomic_t *stop;

    int wake_rd;
    int wake_wr;

    dm_escape_t esc;
    bool command_mode;
    char cmdline[160];
    size_t cmdlen;

    bool interactive;
    bool input_eof;
    bool local_echo;
    bool finished;
    dm_session_end_t reason;

    int64_t started_ms;
    int64_t connected_ms;    /* when CONNECT was printed, 0 if never */
    int64_t eof_ms;          /* when the input ended and the queue drained */

    /* The far end's BYE tears the media down before the loop exits, and
     * the RTP statistics go with it - so keep the last good sample. */
    dm_link_quality_t quality;
    bool have_quality;

    /* Dominance watch on what arrives, see count_repeats(). */
    unsigned hist[256];
    unsigned hist_n;
    bool repeat_warned;
} dm_session_t;

static void wake_cb(void *user)
{
    dm_session_t *s = user;
    unsigned char one = 1;
    ssize_t n;

    do
    {
        n = write(s->wake_wr, &one, 1);
    } while (n < 0 && errno == EINTR);
    /* A full pipe means the main loop already has a wakeup pending. */
}

static void drain_wake(dm_session_t *s)
{
    unsigned char buf[64];

    while (read(s->wake_rd, buf, sizeof(buf)) > 0)
        ;
}

static void finish(dm_session_t *s, dm_session_end_t reason)
{
    if (s->finished)
        return;
    s->finished = true;
    s->reason = reason;
}

/* A demodulator that is not really locked to anything produces one constant
 * symbol, which frames into the same byte value over and over - classically
 * a screen full of 'U' (0x55). It looks like a bad line and is not: it means
 * nothing on the far end is speaking the modulation we are, or there was no
 * carrier there to lock to.
 *
 * Measured as dominance over a window rather than as a run length, because
 * the real thing is not perfectly constant: framing slips punctuate it, so
 * you see long stretches of one byte broken every so often by two or three
 * others. A run-length test misses that completely.
 *
 * Warn-only, and only once. Real data can be monotonous - a file of zeroes
 * would trip this - and being noisy about a guess is worse than being wrong
 * quietly. */
/* 128 rather than 256: a failed V.42 negotiation delivers a couple of
 * hundred bytes of handshake and then the far end hangs up, so a window
 * that needs 256 to fill never reports at all on exactly the case worth
 * reporting. */
#define DM_DOMINANCE_WINDOW 128
#define DM_DOMINANCE_PCT 90

static void count_repeats(dm_session_t *s, const unsigned char *buf, size_t len)
{
    if (s->repeat_warned)
        return;

    for (size_t i = 0; i < len; i++)
    {
        s->hist[buf[i]]++;
        if (++s->hist_n < DM_DOMINANCE_WINDOW)
            continue;

        {
            unsigned best = 0, second = 0;
            int who = 0, who2 = 0;

            for (int b = 0; b < 256; b++)
            {
                if (s->hist[b] > best)       { second = best; who2 = who; best = s->hist[b]; who = b; }
                else if (s->hist[b] > second){ second = s->hist[b]; who2 = b; }
            }

            /* One value dominating means an unlocked demodulator. Two
             * alternating values usually means framing: a stream of HDLC
             * flags read as async characters looks exactly like "?~?~?~". */
            if ((best + second) * 100u / DM_DOMINANCE_WINDOW >= DM_DOMINANCE_PCT)
            {
                bool hdlc = (who == 0x7e || who2 == 0x7e);

                s->repeat_warned = true;
                if (hdlc)
                    DM_WARN("session",
                            "the far end is sending HDLC flags (0x7e) that we are reading as "
                            "characters - it is still running V.42 while this end has fallen "
                            "back to async. Raise --v42-timeout, or set --v42 off if it will "
                            "not negotiate.");
                else if (best * 100u / DM_DOMINANCE_WINDOW >= DM_DOMINANCE_PCT)
                    DM_WARN("session",
                            "%u%% of the last %d bytes from the far end were 0x%02x. That is "
                            "what a demodulator that never locked produces, not data - check "
                            "--modulation matches what is answering, and that nothing is "
                            "transcoding the audio.",
                            best * 100u / DM_DOMINANCE_WINDOW, DM_DOMINANCE_WINDOW,
                            (unsigned) who);
                else
                    DM_WARN("session",
                            "the last %d bytes from the far end were %u%% just 0x%02x and "
                            "0x%02x. That is a framing or modulation mismatch, not data.",
                            DM_DOMINANCE_WINDOW, (best + second) * 100u / DM_DOMINANCE_WINDOW,
                            (unsigned) who, (unsigned) who2);
                return;
            }
            memset(s->hist, 0, sizeof(s->hist));
            s->hist_n = 0;
        }
    }
}

static void print_info(dm_session_t *s)
{
    dm_modem_status_t st;
    char dur[32];
    bool compressed;

    dm_modem_status(s->modem, &st);
    dm_format_duration(dm_now_ms() - s->started_ms, dur, sizeof(dur));

    dm_tty_message("modulation  %s at %d bps", st.modulation, st.bit_rate);
    dm_tty_message("protocol    %s%s", st.protocol,
                   st.v42_fell_back ? " (the far end did not answer V.42)" : "");
    dm_tty_message("carrier     %s%s", st.phase_text, st.retrains ? " (retrained)" : "");
    if (st.retrains)
        dm_tty_message("retrains    %u", st.retrains);
    dm_tty_message("rx level    %.1f dBm0", (double) st.rx_power);
    if (st.train_stage != NULL)
    {
        /* V.32 and V.32bis: what the receiver and echo canceller make of
         * the line. */
        const char *coding = "4 points";

        switch (st.bit_rate)
        {
        case 14400:
            coding = "trellis, 128 points";
            break;
        case 12000:
            coding = "trellis, 64 points";
            break;
        case 9600:
            coding = st.line_trellis ? "trellis, 32 points" : "nonredundant, 16 points";
            break;
        case 7200:
            coding = "trellis, 16 points";
            break;
        default:
            break;
        }
        dm_tty_message("coding      %s%s", coding,
                       (strcmp(st.modulation, "v32bis") == 0 && !st.line_v32bis && st.bit_rate > 0)
                           ? " (the far end is V.32, not V.32bis)"
                           : "");
        if (st.renegotiations)
            dm_tty_message("rate changes %u, without retraining", st.renegotiations);
        dm_tty_message("snr         %.1f dB", (double) st.snr_db);
        if (st.round_trip_ms >= 0)
            dm_tty_message("round trip  %d ms", st.round_trip_ms);
        if (st.echo_cancelling)
            dm_tty_message("echo        %.1f ms back, %.1f dB down, cancelled a further %.1f dB",
                           (double) st.echo_delay_ms, (double) st.echo_return_loss_db,
                           (double) st.echo_cancelled_db);
        else
            dm_tty_message("echo        none heard");
        if (st.phase != DM_PHASE_DATA)
            dm_tty_message("handshake   %s", st.train_stage);
    }
    {
        dm_link_quality_t q;

        if (dm_sip_link_quality(&q) && q.rx_packets > 0)
            dm_tty_message("audio path  %u packets in, %u lost, %u discarded, jitter %.1fms",
                           q.rx_packets, q.rx_lost, q.rx_discarded, q.jitter_us / 1000.0);
    }
    dm_tty_message("duration    %s", dur);

    /* Show what each direction cost on the line only when there is
     * compression to account for - with plain V.42 the two numbers are the
     * same and the ratio is noise. Each direction stands on its own, because
     * a session where only one end has said anything is the normal case. */
    compressed = (strstr(st.protocol, "bis") != NULL);

    if (compressed && st.wire_tx > 0)
        dm_tty_message("sent        %" PRIu64 " bytes, %" PRIu64 " on the wire (%.2f:1)", st.bytes_tx,
                       st.wire_tx, (double) st.bytes_tx / (double) st.wire_tx);
    else
        dm_tty_message("sent        %" PRIu64 " bytes", st.bytes_tx);

    if (compressed && st.wire_rx > 0)
        dm_tty_message("received    %" PRIu64 " bytes, %" PRIu64 " on the wire (%.2f:1)", st.bytes_rx,
                       st.wire_rx, (double) st.bytes_rx / (double) st.wire_rx);
    else
        dm_tty_message("received    %" PRIu64 " bytes", st.bytes_rx);
    if (st.frame_errors)
        dm_tty_message("corrected   %u damaged frame%s", st.frame_errors,
                       st.frame_errors == 1 ? "" : "s");
    if (st.tx_dropped || st.rx_dropped)
        dm_tty_message("dropped     %zu out, %zu in", st.tx_dropped, st.rx_dropped);
}

static void print_settings(dm_session_t *s)
{
    const dm_config_t *cfg = s->cfg;

    dm_tty_message("E%d  local echo", s->local_echo ? 1 : 0);
    if (s->esc.escape_char >= 0 && s->esc.escape_char <= 127)
        dm_tty_message("S2=%d  escape character ('%c')", s->esc.escape_char, (char) s->esc.escape_char);
    else
        dm_tty_message("S2=%d  escape character (disabled)", s->esc.escape_char);
    dm_tty_message("S12=%d  escape guard time, ms", s->esc.guard_ms);
    dm_tty_message("format  %d data bits, %s parity, %d stop bit%s", cfg->data_bits, cfg->parity,
                   cfg->stop_bits, cfg->stop_bits == 1 ? "" : "s");
}

static void enter_command_mode(dm_session_t *s)
{
    s->command_mode = true;
    s->cmdlen = 0;
    dm_tty_control("\r\n", 2);
    dm_tty_message("OK");
    DM_DEBUG("session", "escaped to command mode");
}

static void return_online(dm_session_t *s)
{
    dm_modem_status_t st;

    dm_modem_status(s->modem, &st);
    s->command_mode = false;
    s->cmdlen = 0;
    /* Restart the guard timer, or a "+++" typed straight after ATO would see
     * the silence that the command-mode conversation left behind. */
    dm_escape_init(&s->esc, s->cfg->escape_char, s->cfg->escape_guard_ms);
    dm_tty_message("CONNECT %d", st.bit_rate);
}

static bool set_s_register(dm_session_t *s, const char *arg)
{
    int reg;
    int value;
    char *end;

    reg = (int) strtol(arg, &end, 10);
    if (end == arg)
        return false;
    if (*end == '?')
    {
        if (reg == 2)
            dm_tty_message("%d", s->esc.escape_char);
        else if (reg == 12)
            dm_tty_message("%d", s->esc.guard_ms);
        else
            return false;
        return true;
    }
    if (*end != '=')
        return false;
    arg = end + 1;
    value = (int) strtol(arg, &end, 10);
    if (end == arg || *end != '\0')
        return false;

    if (reg == 2)
    {
        if (value < 0 || value > 255)
            return false;
        s->esc.escape_char = value;
    }
    else if (reg == 12)
    {
        /* S12 is in fiftieths of a second on real hardware; milliseconds are
         * more use here and the flag is named --escape-guard-ms to say so. */
        if (value < 100 || value > 10000)
            return false;
        s->esc.guard_ms = value;
    }
    else
    {
        return false;
    }
    return true;
}

/* Returns false when the command asked to end the session. */
static bool run_at_command(dm_session_t *s, char *line)
{
    char *p = line;

    /* Strip whitespace everywhere: real modems ignore it inside a command. */
    {
        char *w = line;
        for (char *r = line; *r != '\0'; r++)
        {
            if (!isspace((unsigned char) *r))
                *w++ = *r;
        }
        *w = '\0';
    }

    if (*p == '\0')
        return true; /* a bare newline gets nothing, like a real modem */

    if (strncasecmp(p, "AT", 2) != 0)
    {
        dm_tty_message("ERROR");
        return true;
    }
    p += 2;

    if (*p == '\0')
    {
        dm_tty_message("OK");
        return true;
    }

    if (strcasecmp(p, "H") == 0 || strcasecmp(p, "H0") == 0 || strcasecmp(p, "Z") == 0)
    {
        dm_tty_message("OK");
        finish(s, DM_END_HANGUP);
        return false;
    }
    if (strcasecmp(p, "O") == 0 || strcasecmp(p, "O0") == 0)
    {
        if (!dm_modem_connected(s->modem))
        {
            dm_tty_message("NO CARRIER");
            finish(s, DM_END_NO_CARRIER);
            return false;
        }
        return_online(s);
        return true;
    }
    if (toupper((unsigned char) p[0]) == 'I' && (p[1] == '\0' || isdigit((unsigned char) p[1])))
    {
        print_info(s);
        dm_tty_message("OK");
        return true;
    }
    if (strcasecmp(p, "&V") == 0)
    {
        print_settings(s);
        dm_tty_message("OK");
        return true;
    }
    if (strcasecmp(p, "E0") == 0 || strcasecmp(p, "E1") == 0)
    {
        s->local_echo = (p[1] == '1');
        dm_tty_message("OK");
        return true;
    }
    if (toupper((unsigned char) p[0]) == 'S')
    {
        if (set_s_register(s, p + 1))
            dm_tty_message("OK");
        else
            dm_tty_message("ERROR");
        return true;
    }
    if (strcasecmp(p, "?") == 0 || strcasecmp(p, "HELP") == 0)
    {
        dm_tty_message("ATO   return to the call      ATH   hang up");
        dm_tty_message("ATI   session info            AT&V  settings");
        dm_tty_message("ATE0/ATE1  local echo         ATS2=n ATS12=n  escape char / guard ms");
        dm_tty_message("OK");
        return true;
    }

    dm_tty_message("ERROR");
    return true;
}

/* Line editing while in command mode: enough to type an AT command and fix a
 * typo, and nothing more. */
static bool command_mode_byte(dm_session_t *s, unsigned char c)
{
    if (c == '\r' || c == '\n')
    {
        dm_tty_control("\r\n", 2);
        s->cmdline[s->cmdlen] = '\0';
        s->cmdlen = 0;
        return run_at_command(s, s->cmdline);
    }
    if (c == 0x08 || c == 0x7f)
    {
        if (s->cmdlen > 0)
        {
            s->cmdlen--;
            dm_tty_control("\b \b", 3);
        }
        return true;
    }
    if (c == 0x04) /* ctrl-D: give up on the call */
    {
        dm_tty_message("");
        finish(s, DM_END_HANGUP);
        return false;
    }
    if (c == 0x15) /* ctrl-U */
    {
        while (s->cmdlen > 0)
        {
            s->cmdlen--;
            dm_tty_control("\b \b", 3);
        }
        return true;
    }
    if (c < 0x20 || c == 0x7f)
        return true;
    if (s->cmdlen + 1 < sizeof(s->cmdline))
    {
        s->cmdline[s->cmdlen++] = (char) c;
        dm_tty_control(&c, 1);
    }
    return true;
}

/* ------------------------------------------------------------- the loop */

static void handle_input(dm_session_t *s, const unsigned char *buf, size_t len)
{
    int64_t now = dm_now_ms();

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = buf[i];
        unsigned char out[4];
        int n;

        if (s->command_mode)
        {
            if (!command_mode_byte(s, c))
                return;
            continue;
        }

        if (s->cfg->escape_key >= 0 && (int) c == s->cfg->escape_key)
        {
            enter_command_mode(s);
            continue;
        }

        if (s->local_echo)
            dm_tty_control(&c, 1);

        n = dm_escape_feed(&s->esc, now, c, out);
        if (n > 0)
        {
            size_t taken = dm_modem_send(s->modem, out, (size_t) n);
            if (taken < (size_t) n)
            {
                /* Should not happen: the loop stops reading stdin before the
                 * queue is this full. Say so rather than silently truncate. */
                DM_WARN("session", "transmit queue overflowed, %zu byte(s) lost", (size_t) n - taken);
            }
        }
    }
}

/* Losing the carrier once a session has been running is an ordinary way for
 * a call to end, and says nothing about whether the work succeeded - exit 0.
 * Losing it before the link was ever usable is a failed call: nothing was
 * ever carried, and a script needs to be able to tell those apart. */
static int exit_code_for(dm_session_end_t reason, bool ever_connected)
{
    switch (reason)
    {
    case DM_END_HANGUP:
    case DM_END_IDLE:
    case DM_END_SIGNAL:
        return DM_EXIT_OK;
    case DM_END_NO_CARRIER:
    case DM_END_REMOTE_CLEARED:
        return ever_connected ? DM_EXIT_OK : DM_EXIT_NO_CARRIER;
    case DM_END_UNSTABLE:
        return DM_EXIT_NO_CARRIER;
    case DM_END_TRAIN_FAILED:
        return DM_EXIT_NO_CARRIER;
    case DM_END_TIMEOUT:
        return DM_EXIT_TIMEOUT;
    case DM_END_ERROR:
        return DM_EXIT_INTERNAL;
    }
    return DM_EXIT_INTERNAL;
}

int dm_session_run(const dm_config_t *cfg, dm_modem_t *modem, volatile sig_atomic_t *stop,
                   dm_session_result_t *result)
{
    dm_session_t s;
    dm_modem_status_t st;
    int pipefd[2];
    int64_t train_deadline;
    char dur[32];

    memset(result, 0, sizeof(*result));
    memset(&s, 0, sizeof(s));
    s.cfg = cfg;
    s.modem = modem;
    s.stop = stop;
    s.started_ms = dm_now_ms();
    s.interactive = dm_tty_is_interactive();
    s.local_echo = cfg->local_echo;
    s.reason = DM_END_ERROR;
    dm_escape_init(&s.esc, cfg->escape_char, cfg->escape_guard_ms);

    if (pipe(pipefd) != 0)
    {
        DM_ERROR("session", "pipe: %s", strerror(errno));
        return DM_EXIT_INTERNAL;
    }
    s.wake_rd = pipefd[0];
    s.wake_wr = pipefd[1];
    fcntl(s.wake_rd, F_SETFL, O_NONBLOCK);
    fcntl(s.wake_wr, F_SETFL, O_NONBLOCK);
    dm_modem_set_wake(modem, wake_cb, &s);

    train_deadline = s.started_ms + (int64_t) cfg->train_timeout_s * 1000;

    for (;;)
    {
        struct pollfd fds[2];
        int nfds = 0;
        int stdin_slot = -1;
        int64_t now;
        unsigned char rxbuf[4096];
        size_t got;
        unsigned char held[4];
        int held_len = 0;
        size_t tx_space = dm_modem_tx_space(modem);

        if (*stop != 0)
        {
            finish(&s, DM_END_SIGNAL);
            break;
        }

        /* Read from the terminal only once there is a carrier to send into
         * and room in the queue to hold it. Not polling stdin is the whole
         * of our flow control: the tty keeps the keystrokes until we ask.
         * The queue holds a few seconds of line time, which at 300 bps is
         * a couple of hundred bytes - so the test has to be against what is
         * actually free, not against a fixed read size. */
        if (s.connected_ms != 0 && !s.input_eof && tx_space > 0)
        {
            fds[nfds].fd = STDIN_FILENO;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            stdin_slot = nfds++;
        }
        fds[nfds].fd = s.wake_rd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        if (poll(fds, (nfds_t) nfds, DM_POLL_MS) < 0 && errno != EINTR)
        {
            DM_ERROR("session", "poll: %s", strerror(errno));
            finish(&s, DM_END_ERROR);
            break;
        }

        now = dm_now_ms();

        if (stdin_slot >= 0 && (fds[stdin_slot].revents & (POLLIN | POLLHUP)))
        {
            unsigned char in[DM_READ_CHUNK];
            size_t want = (tx_space < sizeof(in)) ? tx_space : sizeof(in);
            ssize_t n = read(STDIN_FILENO, in, want);

            if (n > 0)
            {
                handle_input(&s, in, (size_t) n);
            }
            else if (n == 0)
            {
                s.input_eof = true;
                DM_DEBUG("session", "local input ended");
            }
            else if (errno != EINTR && errno != EAGAIN)
            {
                DM_WARN("session", "read from the terminal failed: %s", strerror(errno));
                s.input_eof = true;
            }
        }
        if (s.finished)
            break;

        drain_wake(&s);

        /* The escape sequence completes on silence, so it can only be
         * noticed here rather than on a keystroke. */
        if (!s.command_mode && dm_escape_tick(&s.esc, now, held, &held_len))
        {
            enter_command_mode(&s);
        }
        else if (held_len > 0)
        {
            dm_modem_send(modem, held, (size_t) held_len);
        }

        /* ---- state checks ---- */

        dm_modem_status(modem, &st);
        if (dm_sip_link_quality(&s.quality))
            s.have_quality = true;

        /* Wait for a usable link rather than merely a carrier: with V.42
         * asked for there is a handshake still to run, and announcing
         * CONNECT before it finishes would invite the user to type into a
         * link that cannot carry it yet. */
        if (s.connected_ms == 0 && st.data_ready)
        {
            s.connected_ms = now;
            result->bit_rate = st.bit_rate;
            if (strcmp(st.protocol, "async") == 0)
                dm_tty_message("CONNECT %d", st.bit_rate);
            else
                dm_tty_message("CONNECT %d %s", st.bit_rate, st.protocol);
            dm_log_event(DM_LOG_INFO, "session", "connected",
                         "rate=%d modulation=%s protocol=%s train_ms=%" PRId64, st.bit_rate,
                         st.modulation, st.protocol, now - s.started_ms);
            if (st.v42_fell_back)
                DM_WARN("session", "the far end did not answer V.42; this link has no error "
                                   "correction");
            if (s.interactive)
            {
                char esc[8];
                dm_format_byte(cfg->escape_char, esc, sizeof(esc));
                DM_INFO("session", "type %s%s%s (with a %dms pause either side) for command mode", esc, esc,
                        esc, cfg->escape_guard_ms);
            }
        }

        /* Received characters go straight to the screen, except in command
         * mode: there they queue up behind the AT conversation and arrive
         * when ATO puts us back online.
         *
         * Deliberately after the CONNECT announcement above and not before
         * it: the far end can have bytes waiting in the same pass that the
         * link comes up, and printing them first produces a CONNECT line
         * with data already run into the front of it. */
        if (!s.command_mode)
        {
            while ((got = dm_modem_recv(modem, rxbuf, sizeof(rxbuf))) > 0)
            {
                count_repeats(&s, rxbuf, got);
                if (!dm_tty_write(rxbuf, got))
                {
                    DM_WARN("session", "cannot write to stdout, hanging up");
                    finish(&s, DM_END_HANGUP);
                    break;
                }
                if (got < sizeof(rxbuf))
                    break;
            }
        }
        if (s.finished)
            break;

        if (s.connected_ms == 0 && now > train_deadline)
        {
            dm_tty_message("NO CARRIER");
            DM_ERROR("session", "the modems did not train within %ds (phase: %s)", cfg->train_timeout_s,
                     st.phase_text);
            finish(&s, DM_END_TRAIN_FAILED);
            break;
        }

        if (st.carrier_lost)
        {
            dm_tty_message("NO CARRIER");
            finish(&s, DM_END_NO_CARRIER);
            break;
        }

        if (dm_sip_call_ended())
        {
            dm_tty_message("NO CARRIER");
            DM_INFO("session", "the call was cleared by the far end");
            finish(&s, DM_END_REMOTE_CLEARED);
            break;
        }

        if (cfg->media_timeout_s > 0)
        {
            int64_t since = dm_sip_since_rtp_ms();
            if (since >= 0 && since > (int64_t) cfg->media_timeout_s * 1000)
            {
                dm_tty_message("NO CARRIER");
                DM_ERROR("session", "no RTP from the far end for %ds - check the media path",
                         cfg->media_timeout_s);
                finish(&s, DM_END_TIMEOUT);
                break;
            }
        }

        /* A link that keeps retraining is not going to settle. The usual
         * cause is the two ends disagreeing about the rate: V.22bis says
         * "I can do 2400" by sending S1 during training, and an end that
         * misses it settles at 1200 while the far end stays at 2400. Each
         * then demodulates the other wrongly, so both see noise, and the
         * far end keeps asking to retrain. Say so and stop, rather than
         * filling the terminal with garbage until somebody clears the
         * call. */
        if (cfg->max_retrains > 0 && st.retrains >= (unsigned) cfg->max_retrains)
        {
            dm_tty_message("NO CARRIER");
            if (st.train_stage != NULL)
                /* V.32 agrees its rate explicitly, so the ends cannot
                 * disagree about it the way V.22bis's can; and each retrain
                 * already picks the rate the line will bear. One that still
                 * will not hold has a line problem. */
                DM_ERROR("session",
                         "the link retrained %u times at %d bps and will not hold (%.1f dB SNR) - "
                         "the audio path is too poor for %s. --modulation v22bis or v21 ask far "
                         "less of it.",
                         st.retrains, st.bit_rate, (double) st.snr_db,
                         strcmp(st.modulation, "v32bis") == 0 ? "V.32bis" : "V.32");
            else if (st.offered_rate > 0 && st.bit_rate < st.offered_rate)
                DM_ERROR("session",
                         "the link retrained %u times and will not hold. It settled at %d bps "
                         "having offered %d, which usually means the two ends disagreed about "
                         "the rate - try --bit-rate %d so both start there, or --modulation v21, "
                         "which is far more tolerant of a poor audio path.",
                         st.retrains, st.bit_rate, st.offered_rate, st.bit_rate);
            else
                DM_ERROR("session",
                         "the link retrained %u times at %d bps and will not hold - check the "
                         "audio path, and try --modulation v21, which is far more tolerant of "
                         "a poor one.",
                         st.retrains, st.bit_rate);
            finish(&s, DM_END_UNSTABLE);
            break;
        }

        if (cfg->max_call_s > 0 && now - s.started_ms > (int64_t) cfg->max_call_s * 1000)
        {
            DM_WARN("session", "maximum call length of %ds reached", cfg->max_call_s);
            finish(&s, DM_END_TIMEOUT);
            break;
        }

        if (cfg->idle_timeout_s > 0 && s.connected_ms != 0 &&
            dm_modem_since_rx_ms(modem) > (int64_t) cfg->idle_timeout_s * 1000 &&
            dm_modem_drained(modem))
        {
            DM_INFO("session", "nothing said either way for %ds", cfg->idle_timeout_s);
            finish(&s, DM_END_IDLE);
            break;
        }

        /* Unattended run: the script's input has ended, everything we had to
         * say is on the wire, so wait out the reply and then clear the call. */
        if (s.input_eof && !s.command_mode && dm_modem_drained(modem))
        {
            int idle_s = cfg->idle_timeout_s > 0 ? cfg->idle_timeout_s : DM_EOF_IDLE_S;

            if (s.eof_ms == 0)
            {
                s.eof_ms = now;
                DM_INFO("session", "input finished and the queue is drained; "
                                   "hanging up after %ds without a reply",
                        idle_s);
            }
            if (dm_modem_since_rx_ms(modem) > (int64_t) idle_s * 1000 &&
                now - s.eof_ms > (int64_t) idle_s * 1000)
            {
                finish(&s, DM_END_HANGUP);
                break;
            }
        }
    }

    /* Anything the far end managed to say before the end still belongs on
     * the screen. */
    for (;;)
    {
        unsigned char rxbuf[4096];
        size_t got = dm_modem_recv(modem, rxbuf, sizeof(rxbuf));
        if (got == 0)
            break;
        dm_tty_write(rxbuf, got);
    }

    dm_modem_set_wake(modem, NULL, NULL);
    close(s.wake_rd);
    close(s.wake_wr);

    dm_modem_status(modem, &st);
    result->reason = s.reason;
    result->duration_ms = dm_now_ms() - s.started_ms;
    result->connected_ms = s.connected_ms ? dm_now_ms() - s.connected_ms : 0;
    result->bytes_tx = st.bytes_tx;
    result->bytes_rx = st.bytes_rx;
    result->bit_rate = st.bit_rate;

    dm_format_duration(result->connected_ms, dur, sizeof(dur));
    if (s.connected_ms != 0)
        dm_tty_message("%s after %s, %" PRIu64 " bytes sent, %" PRIu64 " bytes received",
                       dm_session_end_name(s.reason), dur, result->bytes_tx, result->bytes_rx);

    {
        dm_link_quality_t q = s.quality;

        if (!dm_sip_link_quality(&q) && s.have_quality)
            q = s.quality;          /* media already gone; use the last sample */
        if (q.rx_packets > 0)
        {
            unsigned bad = q.rx_lost + q.rx_discarded;
            unsigned pct = bad * 100u / (q.rx_packets + bad);

            dm_log_event(DM_LOG_INFO, "session", "audio path",
                         "rx_packets=%u rx_lost=%u rx_discarded=%u rx_reordered=%u tx_packets=%u "
                         "jitter_ms=%.1f jitter_max_ms=%.1f rx_level_dbm0=%.1f",
                         q.rx_packets, q.rx_lost, q.rx_discarded, q.rx_reordered, q.tx_packets,
                         q.jitter_us / 1000.0, q.jitter_max_us / 1000.0, (double) st.rx_power);

            /* A modem is far less forgiving than speech. If the path lost
             * anything at all, say so next to the failure rather than
             * leaving it to be inferred from garbled output. */
            if (pct >= 1)
                DM_WARN("session",
                        "the audio path lost %u%% of its packets - that is a transport problem, "
                        "not a modem one. Check nothing is transcoding away from G.711, and try "
                        "--modulation v21, which survives a path that V.32 and V.22bis cannot.",
                        pct);
        }
    }

    /* Independent of whether the RTP statistics could be sampled: the
     * retrain count comes from the modem, and is worth saying on its own. */
    if (st.retrains >= 2 && s.reason != DM_END_UNSTABLE)
        DM_WARN("session", "the modem retrained %u times during this call - the audio path or "
                           "the agreed rate is marginal",
                st.retrains);

    dm_log_event(DM_LOG_INFO, "session", "finished",
                 "reason=\"%s\" modulation=%s rate=%d protocol=%s connected_ms=%" PRId64
                 " duration_ms=%" PRId64 " bytes_tx=%" PRIu64 " bytes_rx=%" PRIu64 " wire_tx=%" PRIu64
                 " wire_rx=%" PRIu64 " frame_errors=%u retrains=%u dropped_tx=%zu dropped_rx=%zu",
                 dm_session_end_name(s.reason), st.modulation, st.bit_rate, st.protocol,
                 result->connected_ms, result->duration_ms, result->bytes_tx, result->bytes_rx,
                 st.wire_tx, st.wire_rx, st.frame_errors, st.retrains, st.tx_dropped, st.rx_dropped);

    return exit_code_for(s.reason, s.connected_ms != 0);
}
