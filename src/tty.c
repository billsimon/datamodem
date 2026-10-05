#include "datamodem/tty.h"
#include "datamodem/term.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* The terminal as we found it, taken once, before quiet or raw mode first
 * changes it - so that restoring always means restoring that. */
static struct termios g_saved;
static bool g_saved_valid = false;
static volatile sig_atomic_t g_raw = 0;
static bool g_registered = false;

bool dm_tty_is_interactive(void)
{
    return isatty(STDIN_FILENO) == 1;
}

static void restore_atexit(void)
{
    dm_tty_restore();
}

static bool save_once(char *err, size_t err_len)
{
    if (g_saved_valid)
        return true;
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0)
    {
        snprintf(err, err_len, "tcgetattr: %s", strerror(errno));
        return false;
    }
    g_saved_valid = true;
    if (!g_registered)
    {
        atexit(restore_atexit);
        g_registered = true;
    }
    return true;
}

void dm_tty_quiet(void)
{
    struct termios q;
    char err[64];

    if (!dm_tty_is_interactive() || !save_once(err, sizeof(err)))
        return;
    q = g_saved;
    /* No echo and no line editing, so that nothing typed lands on the
     * screen; signals still work, so ctrl-c still stops a call that is
     * dialling. */
    q.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    q.c_cc[VMIN] = 1;
    q.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &q) == 0)
        g_raw = 1;
}

bool dm_tty_raw(char *err, size_t err_len)
{
    struct termios raw;

    if (!dm_tty_is_interactive())
        return true;

    if (!save_once(err, err_len))
        return false;

    raw = g_saved;
    /* No canonical line editing, no local echo (the remote host echoes, or
     * --local-echo does), and no signal generation: ctrl-C is a byte the
     * remote asked for, not an instruction to us. */
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
    /* No CR/LF rewriting, no XON/XOFF interception, no parity or 8th-bit
     * mangling on the way in. */
    raw.c_iflag &= (tcflag_t) ~(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP | INPCK | BRKINT);
    /* No output post-processing: the remote's CR and LF reach the screen as
     * sent, which is why every message of ours writes \r\n explicitly. */
    raw.c_oflag &= (tcflag_t) ~OPOST;
    raw.c_cflag |= CS8;
    /* read() returns as soon as there is anything; poll() decides when to
     * call it. */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        snprintf(err, err_len, "tcsetattr: %s", strerror(errno));
        return false;
    }
    g_raw = 1;
    /* With the screen ours, anything typed while the call was being set up
     * was typed at nothing; a modem would not have sent it either. */
    if (dm_term_active())
        tcflush(STDIN_FILENO, TCIFLUSH);
    return true;
}

void dm_tty_restore(void)
{
    if (!g_raw)
        return;
    g_raw = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
}

static bool write_all(int fd, const void *data, size_t len)
{
    const char *p = data;

    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n > 0)
        {
            p += n;
            len -= (size_t) n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        return false;
    }
    return true;
}

bool dm_tty_write_raw(const void *data, size_t len)
{
    return write_all(STDOUT_FILENO, data, len);
}

/* What the far end sent. Into the emulated screen when it is ours; made safe
 * to print when stdout is a terminal anyway; and untouched when it is a file
 * or a pipe, which wants exactly what came off the line. */
bool dm_tty_write(const void *data, size_t len)
{
    static int is_tty = -1;

    if (dm_term_active())
    {
        dm_term_remote(data, len);
        return true;
    }
    if (is_tty < 0)
        is_tty = isatty(STDOUT_FILENO);
    if (is_tty)
    {
        const unsigned char *p = data;

        while (len > 0)
        {
            char out[DM_TERM_SANITIZE_CAP(1024)];
            size_t chunk = len > 1024 ? 1024 : len;
            size_t n = dm_term_sanitize(p, chunk, out);

            if (n > 0 && !write_all(STDOUT_FILENO, out, n))
                return false;
            p += chunk;
            len -= chunk;
        }
        return true;
    }
    return write_all(STDOUT_FILENO, data, len);
}

/* Resolved once: whether stdout is a terminal cannot change under us, and
 * this is on the path of every echoed keystroke. */
static int control_fd(void)
{
    static int fd = -1;

    if (fd < 0)
        fd = isatty(STDOUT_FILENO) ? STDOUT_FILENO : STDERR_FILENO;
    return fd;
}

bool dm_tty_control(const void *data, size_t len)
{
    if (dm_term_active())
    {
        dm_term_local(data, len);
        return true;
    }
    return write_all(control_fd(), data, len);
}

void dm_tty_message(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    dm_term_fresh_line();
    if ((size_t) n >= sizeof(buf))
        n = (int) sizeof(buf) - 1;

    if ((size_t) n + 2 < sizeof(buf))
    {
        buf[n++] = '\r';
        buf[n++] = '\n';
        dm_tty_control(buf, (size_t) n);
    }
    else
    {
        dm_tty_control(buf, (size_t) n);
        dm_tty_control("\r\n", 2);
    }
}
