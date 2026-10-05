/* A fuzzer for the terminal emulator in src/term.c.
 *
 *     cmake --build build --target term-fuzz && build/term-fuzz [seed] [rounds]
 *
 * Puts the emulator on a pty and feeds it random bytes and random escape
 * sequences - huge parameters, empty ones, private modes - at random window
 * sizes, resizing it as it goes. Built with the address and undefined
 * behaviour sanitizers, so a bad index or a hang (an alarm after five seconds
 * on one chunk) ends the run with a report. The first version of the screen
 * had both, and the end-to-end tests found neither. */
#include "datamodem/config.h"
#include "datamodem/term.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

static int master;

static void *drain(void *arg)
{
    char buf[65536];

    (void) arg;
    while (read(master, buf, sizeof(buf)) > 0)
        continue;
    return NULL;
}

static void reply(void *user, const void *data, size_t len)
{
    (void) user;
    (void) data;
    (void) len;
}

static void on_alarm(int sig)
{
    static const char msg[] = "term-fuzz: a chunk took more than five seconds - a loop\n";

    (void) sig;
    (void) !write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(3);
}

static void random_size(struct winsize *ws)
{
    ws->ws_col = (unsigned short) (20 + rand() % 200);
    ws->ws_row = (unsigned short) (3 + rand() % 80);
}

int main(int argc, char **argv)
{
    static const char *const charsets[] = { "cp437", "utf8", "ascii" };
    static const char finals[] = "ABCDEFGHJKLMPSTXdfmnrsuhl@`c";
    unsigned seed = (argc > 1) ? (unsigned) atoi(argv[1]) : 1;
    int rounds = (argc > 2) ? atoi(argv[2]) : 30;
    struct winsize ws = { 24, 80, 0, 0 };
    dm_config_t cfg;
    pthread_t th;
    int slave;

    if (openpty(&master, &slave, NULL, NULL, &ws) != 0)
    {
        perror("openpty");
        return 1;
    }
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    pthread_create(&th, NULL, drain, NULL);
    signal(SIGALRM, on_alarm);
    srand(seed);
    memset(&cfg, 0, sizeof(cfg));
    cfg.tui = true;

    for (int r = 0; r < rounds; r++)
    {
        snprintf(cfg.charset, sizeof(cfg.charset), "%s", charsets[rand() % 3]);
        random_size(&ws);
        ioctl(master, TIOCSWINSZ, &ws);
        if (!dm_term_start(&cfg))
        {
            fprintf(stderr, "term-fuzz: could not start at %dx%d\n", ws.ws_col, ws.ws_row);
            return 2;
        }
        dm_term_set_reply(reply, NULL);
        for (int k = 0; k < 400; k++)
        {
            unsigned char buf[512];
            size_t n = 0;

            alarm(5);
            while (n < sizeof(buf) - 40)
            {
                int pick = rand() % 10;

                if (pick < 4)
                    buf[n++] = (unsigned char) rand();
                else if (pick < 6)
                    buf[n++] = (unsigned char) (0x20 + rand() % 95);
                else if (pick == 6)
                {
                    buf[n++] = '\r';
                    buf[n++] = '\n';
                }
                else
                    n += (size_t) snprintf((char *) buf + n, 40, "\033[%s%d;%d%c", (rand() % 4) ? "" : "?",
                                           (rand() % 3) ? rand() % 300 : rand(), rand() % 300,
                                           finals[rand() % (int) (sizeof(finals) - 1)]);
            }
            dm_term_remote(buf, n);
            if (rand() % 50 == 0)
                dm_term_local("NO CARRIER\r\n", 12);
            if (rand() % 80 == 0)
            {
                random_size(&ws);
                ioctl(master, TIOCSWINSZ, &ws);
                raise(SIGWINCH);
            }
            dm_term_tick();
        }
        dm_term_stop();
    }
    alarm(0);
    fprintf(stderr, "term-fuzz: seed %u, %d rounds, no faults\n", seed, rounds);
    return 0;
}
