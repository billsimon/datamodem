#include "datamodem/tty.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved;
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

bool dm_tty_raw(char *err, size_t err_len)
{
    struct termios raw;

    if (!dm_tty_is_interactive())
        return true;

    if (tcgetattr(STDIN_FILENO, &g_saved) != 0)
    {
        snprintf(err, err_len, "tcgetattr: %s", strerror(errno));
        return false;
    }

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

    if (!g_registered)
    {
        atexit(restore_atexit);
        g_registered = true;
    }
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

bool dm_tty_write(const void *data, size_t len)
{
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
    if ((size_t) n >= sizeof(buf))
        n = (int) sizeof(buf) - 1;

    dm_tty_control(buf, (size_t) n);
    dm_tty_control("\r\n", 2);
}
