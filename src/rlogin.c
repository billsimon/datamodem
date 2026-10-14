#include "datamodem/rlogin.h"
#include "datamodem/hayes.h"
#include "datamodem/log.h"
#include "datamodem/modem.h"
#include "datamodem/session.h"
#include "datamodem/sip.h"
#include "datamodem/tty.h"
#include "datamodem/util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RLOGIN_POLL_MS 100

/* RFC 1282 gives a client no deadline for its handshake; a connection that
 * has sent none of it by then is not an RLogin client. */
#define RLOGIN_HANDSHAKE_MS 10000

/* Longer than any user name SyncTERM keeps (30 characters, or 128 for the
 * password that RLogin Reversed sends in its place). */
#define RLOGIN_FIELD_MAX 256

typedef struct
{
    const dm_config_t *cfg;
    volatile sig_atomic_t *stop;
    int listen_fd;
    bool sip_started;

    /* While a call is being dialled: the connection, which a thread
     * watches for closing, and the flag that abandons the call. */
    int conn_fd;
    volatile sig_atomic_t dialling;
    volatile sig_atomic_t abandon;

    /* While a connection is ours: the thread that turns the others away. */
    volatile sig_atomic_t busy;
} rlogin_t;

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
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

/* Closes a connection without losing what we last said on it: a close with
 * the client's typing still unread would reset the connection, and the reset
 * can overtake the NO CARRIER on its way out. So stop sending, and wait a
 * moment for the client to close its side. */
static void close_gently(int fd)
{
    int64_t deadline = dm_now_ms() + 1000;

    shutdown(fd, SHUT_WR);
    while (dm_now_ms() < deadline)
    {
        struct pollfd p = {fd, POLLIN, 0};
        char buf[256];

        if (poll(&p, 1, 100) > 0 && read(fd, buf, sizeof(buf)) <= 0)
            break;
    }
    close(fd);
}

static void describe_peer(int fd, char *out, size_t out_len)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];

    if (getpeername(fd, (struct sockaddr *) &ss, &len) != 0 ||
        getnameinfo((struct sockaddr *) &ss, len, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    {
        snprintf(out, out_len, "%s", "somewhere");
        return;
    }
    snprintf(out, out_len, "%s port %s", host, serv);
}

static int open_listener(const dm_config_t *cfg)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port[16];
    int fd = -1;
    int err = 0;
    int one = 1;
    int gai;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    snprintf(port, sizeof(port), "%d", cfg->rlogin_port);
    gai = getaddrinfo(cfg->rlogin_bind[0] != '\0' ? cfg->rlogin_bind : NULL, port, &hints, &res);
    if (gai != 0)
    {
        DM_ERROR("rlogin", "cannot listen on %s: %s", cfg->rlogin_bind, gai_strerror(gai));
        return -1;
    }
    for (struct addrinfo *a = res; a != NULL && fd < 0; a = a->ai_next)
    {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
        {
            err = errno;
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, a->ai_addr, a->ai_addrlen) != 0 || listen(fd, 4) != 0)
        {
            err = errno;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0)
    {
        DM_ERROR("rlogin", "cannot listen on %s port %d: %s", cfg->rlogin_bind, cfg->rlogin_port,
                 strerror(err));
        if (err == EACCES && cfg->rlogin_port < 1024)
            DM_ERROR("rlogin", "ports below 1024 need root here; --rlogin-port 5513 is one that "
                               "does not, with the same port in each SyncTERM entry's address "
                               "(localhost:5513)");
        else if (err == EADDRINUSE)
            DM_ERROR("rlogin", "something else - another datamodem, or an rlogin server - is "
                               "listening there already; --rlogin-port picks another");
        return -1;
    }
    /* Non-blocking so that a connection gone between poll() and accept()
     * cannot hold us in accept(). */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static int accept_one(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    int one = 1;

    if (fd < 0)
        return -1;
    /* Some systems hand the listener's O_NONBLOCK on; the session wants a
     * descriptor that blocks, as a terminal does. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    /* Every keystroke on its way at once, not gathered up for later. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* The RFC 1282 handshake: a NUL, then three NUL-terminated strings - the
 * client's user name, the server's, and the terminal type with its speed.
 * SyncTERM sends the entry's password as the first and its user name as the
 * second, or the other way about for RLogin Reversed. */
static bool read_handshake(int fd, char fields[3][RLOGIN_FIELD_MAX])
{
    int64_t deadline = dm_now_ms() + RLOGIN_HANDSHAKE_MS;
    int field = -1; /* the leading NUL first */
    size_t len = 0;

    while (field < 3)
    {
        struct pollfd p = {fd, POLLIN, 0};
        int64_t left = deadline - dm_now_ms();
        unsigned char c;
        ssize_t n;

        if (left <= 0)
            return false;
        if (poll(&p, 1, (int) (left < RLOGIN_POLL_MS ? left : RLOGIN_POLL_MS)) <= 0)
            continue;
        n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        if (field < 0)
        {
            if (c != '\0')
                return false;
            field = 0;
            continue;
        }
        if (c == '\0')
        {
            fields[field][len] = '\0';
            field++;
            len = 0;
        }
        else if (len + 1 < RLOGIN_FIELD_MAX)
        {
            fields[field][len++] = (char) c;
        }
    }
    return true;
}

/* While a call is up, any other connection is told so and let go: there is
 * the one SIP call at a time. */
static void *turn_away(void *arg)
{
    rlogin_t *r = arg;
    static const char msg[] = "\0datamodem is on another call; try again when it has finished\r\n";

    while (r->busy)
    {
        struct pollfd p = {r->listen_fd, POLLIN, 0};
        int fd;

        if (poll(&p, 1, RLOGIN_POLL_MS) <= 0)
            continue;
        fd = accept_one(r->listen_fd);
        if (fd < 0)
            continue;
        DM_INFO("rlogin", "another connection while a call is up; turned away");
        write_all(fd, msg, sizeof(msg) - 1);
        close_gently(fd);
    }
    return NULL;
}

/* While the call is being dialled: the connection closing - SyncTERM
 * giving up - abandons it. Typing, meanwhile, is typing at nothing. */
static void *dial_watch(void *arg)
{
    rlogin_t *r = arg;

    while (r->dialling)
    {
        struct pollfd p = {r->conn_fd, POLLIN, 0};
        unsigned char buf[256];
        ssize_t n;

        if (*r->stop != 0)
        {
            r->abandon = 1;
            break;
        }
        if (poll(&p, 1, 50) <= 0)
            continue;
        n = read(r->conn_fd, buf, sizeof(buf));
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n <= 0)
        {
            DM_INFO("rlogin", "the connection closed while dialling; the call is abandoned");
            r->abandon = 1;
            break;
        }
    }
    return NULL;
}

static void call(rlogin_t *r, const char *number)
{
    dm_config_t cfg = *r->cfg;
    dm_modem_params_t params;
    dm_modem_t *modem;
    dm_session_result_t sr;
    dm_call_result_t cr;
    pthread_t watcher;
    bool watching;
    int rc;

    /* The connection ending is the terminal program hanging up: there is
     * no reply to wait for. */
    cfg.hangup_on_eof = true;

    if (!r->sip_started)
    {
        rc = dm_sip_start(&cfg);
        if (rc != DM_EXIT_OK)
        {
            dm_sip_stop();
            dm_tty_result("%s", dm_hayes_dial_failure(rc, 0));
            return;
        }
        r->sip_started = true;
    }

    dm_modem_params_from_config(&cfg, true, "out", &params);
    modem = dm_modem_create(&params);
    if (modem == NULL)
    {
        dm_tty_result("ERROR");
        return;
    }

    r->abandon = 0;
    r->dialling = 1;
    watching = (pthread_create(&watcher, NULL, dial_watch, r) == 0);
    rc = dm_sip_dial(&cfg, number, modem, watching ? &r->abandon : r->stop);
    r->dialling = 0;
    if (watching)
        pthread_join(watcher, NULL);

    if (rc != DM_EXIT_OK)
    {
        dm_sip_hangup(&cr);
        dm_modem_destroy(modem);
        if (!r->abandon)
            dm_tty_result("%s", dm_hayes_dial_failure(rc, cr.sip_status));
        return;
    }

    dm_session_run(&cfg, modem, r->stop, NULL, &sr);
    dm_sip_hangup(&cr);
    dm_modem_destroy(modem);
    dm_log_event(DM_LOG_INFO, "call", "cleared", "remote=\"%s\" sip_status=%d sip_reason=\"%s\" duration_ms=%d",
                 cr.remote_uri, cr.sip_status, cr.sip_reason, cr.duration_ms);
}

/* One connection, from its handshake to its close. */
static void serve(rlogin_t *r, int fd)
{
    char fields[3][RLOGIN_FIELD_MAX];
    char peer[NI_MAXHOST + 32];
    char number[DM_STR_MAX];
    const char *dial;
    pthread_t refuser;
    bool refusing;
    int null_fd;

    describe_peer(fd, peer, sizeof(peer));
    if (!read_handshake(fd, fields))
    {
        DM_WARN("rlogin", "a connection from %s that did not start as RLogin does; closed", peer);
        close(fd);
        return;
    }
    /* The NUL that says the handshake was taken. */
    write_all(fd, "", 1);

    /* The number is whichever user name has one: the server's, which is
     * SyncTERM's Username field - or its Password, for RLogin Reversed and
     * an address too long for Username - and otherwise the client's. */
    dial = fields[1][0] != '\0' ? fields[1] : fields[0];
    while (*dial == ' ')
        dial++;
    DM_INFO("rlogin", "a connection from %s (terminal %s), to dial %s", peer,
            fields[2][0] != '\0' ? fields[2] : "unnamed", dial[0] != '\0' ? dial : "nothing");
    if (!dm_hayes_number(dial, number, sizeof(number)))
    {
        static const char msg[] = "datamodem: no number to dial. Put the number - or a SIP URI - in "
                                  "this entry's Username.\r\n";

        write_all(fd, msg, sizeof(msg) - 1);
        close_gently(fd);
        return;
    }

    r->busy = 1;
    refusing = (pthread_create(&refuser, NULL, turn_away, r) == 0);

    /* From here the connection is our stdin and stdout, as the port is for
     * `datamodem modem`, and the session runs on it unchanged. The log
     * stays on stderr. */
    if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0)
    {
        DM_ERROR("rlogin", "dup2: %s", strerror(errno));
    }
    else
    {
        r->conn_fd = STDIN_FILENO;
        call(r, number);
    }

    /* Hung up, whichever end did it: let the connection go, leaving stdin
     * and stdout on nothing until the next. */
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0)
    {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        close(null_fd);
    }
    /* The call is over, so the next connection is the main loop's again,
     * even while this one takes its moment to close. */
    r->busy = 0;
    if (refusing)
        pthread_join(refuser, NULL);
    close_gently(fd);
}

int dm_rlogin_run(const dm_config_t *cfg, volatile sig_atomic_t *stop)
{
    static rlogin_t r;

    memset(&r, 0, sizeof(r));
    r.cfg = cfg;
    r.stop = stop;
    r.listen_fd = open_listener(cfg);
    if (r.listen_fd < 0)
        return DM_EXIT_CONFIG;

    /* The connection is a line to a terminal program: what comes off the
     * call goes on it byte for byte, and so do the modem's words, RINGING
     * among them. */
    dm_tty_set_line(true);

    DM_INFO("rlogin", "taking RLogin connections on %s port %d: each dials the number in its user name",
            cfg->rlogin_bind, cfg->rlogin_port);

    /* Up now, rather than at the first connection, so that a registration
     * has already happened by the time anyone dials. A failure is not the
     * end: the next connection tries again, and says NO DIALTONE if that
     * fails too. */
    if (dm_sip_start(cfg) == DM_EXIT_OK)
        r.sip_started = true;
    else
        dm_sip_stop();

    while (*stop == 0)
    {
        struct pollfd p = {r.listen_fd, POLLIN, 0};
        int fd;

        if (poll(&p, 1, RLOGIN_POLL_MS) <= 0)
            continue;
        fd = accept_one(r.listen_fd);
        if (fd >= 0)
            serve(&r, fd);
    }

    close(r.listen_fd);
    if (r.sip_started)
        dm_sip_stop();
    return DM_EXIT_OK;
}
