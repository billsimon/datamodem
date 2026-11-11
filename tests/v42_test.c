/* Unit tests for the vendored V.42 (third_party/spandsp-v42): its
 * per-context state (patch 13) - a context takes a slot in the side table and
 * gives it back when it is freed, so that the ninth link in a process, and
 * the hundredth, has one of its own and starts from nothing - and how long a
 * link that is up waits for an answer before giving up (patch 14). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* For a v42_state_t in our own memory, as the last test needs. */
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

extern void dm_v42_set_t401(const v42_state_t *s, int ms);
extern int dm_v42_xid_done(const v42_state_t *s);
extern int dm_v42_xid_slots_in_use(void);
extern char dm_v42_disconnect_cause[80];

static int failures;

static int frame_get(void *user, uint8_t *msg, int max_len)
{
    return 0;
}

static void frame_put(void *user, const uint8_t *msg, int len)
{
}

static void check_slots(const char *when, int want)
{
    int got = dm_v42_xid_slots_in_use();

    if (got != want)
    {
        printf("FAIL %s: %d slots in use, want %d\n", when, got, want);
        failures++;
    }
}

/* What modem.c does for each call: a context per link, its T401 sized - the
 * first lookup, which takes a slot - and freed when the call ends. */
static v42_state_t *open_link(int calling)
{
    v42_state_t *s = v42_init(NULL, calling, 1, frame_get, frame_put, NULL);

    if (s != NULL)
        dm_v42_set_t401(s, 1500);
    return s;
}

/* Two links joined by a bit pipe at 9600 bit/s, which can be cut: everything
 * either sends lost, as on a line that has gone bad. The caller always has
 * something to send, so its T401 is running. Both ways, because the cause is
 * one variable for the whole process - datamodem runs one link at a time -
 * and an answerer that heard the caller's DISC would say why it went down
 * before the caller had finished. */
typedef struct
{
    int connected;
    int disconnected;
    char why[80];             /* the cause, as it stood when the link went down */
} link_seen_t;

static int busy_get(void *user, uint8_t *msg, int max_len)
{
    int n = (max_len < 64) ? max_len : 64;

    memset(msg, 'x', (size_t) n);
    return n;
}

static void link_status(void *user, int status)
{
    link_seen_t *seen = user;

    if (status == SIG_STATUS_LINK_CONNECTED)
        seen->connected++;
    else if (status == SIG_STATUS_LINK_DISCONNECTED && seen->disconnected++ == 0)
        snprintf(seen->why, sizeof(seen->why), "%s", dm_v42_disconnect_cause);
}

/* Bring a link up, cut the line for cut_s seconds, and run on for another
 * 30. True if the caller's link was still up at the end;
 * why says what it said if not. */
static int survives_cut(double cut_s, char *why, size_t why_len)
{
    const long rate = 9600;
    link_seen_t ca = { 0 }, an = { 0 };
    v42_state_t *a = v42_init(NULL, 1, 1, busy_get, frame_put, NULL);
    v42_state_t *b = v42_init(NULL, 0, 1, frame_get, frame_put, NULL);
    long up_at = -1;
    long end = 0;
    int ok;

    a->tx_bit_rate = b->tx_bit_rate = (int) rate;
    dm_v42_set_t401(a, 1000);
    dm_v42_set_t401(b, 1000);
    v42_set_status_callback(a, link_status, &ca);
    v42_set_status_callback(b, link_status, &an);
    /* As modem.c's v42_arm(): the restart latches the timers from the rate. */
    v42_restart(a);
    v42_restart(b);
    for (long t = 0; t < 120 * rate; t++)
    {
        int ab = v42_tx_bit(a);
        int ba = v42_tx_bit(b);
        int cut = up_at >= 0 && t >= up_at + rate && t < up_at + rate + (long) (cut_s * rate);

        v42_rx_bit(b, cut ? 1 : ab);
        v42_rx_bit(a, cut ? 1 : ba);
        if (up_at < 0 && ca.connected && an.connected)
        {
            up_at = t;
            end = t + rate + (long) (cut_s * rate) + 30 * rate;
        }
        if (up_at >= 0 && t >= end)
            break;
    }
    ok = up_at >= 0 && ca.disconnected == 0;
    snprintf(why, why_len, "%s", up_at < 0 ? "never came up" : ca.why);
    v42_free(a);
    v42_free(b);
    return ok;
}

int main(void)
{
    char when[64];

    check_slots("before any link", 0);

    /* Calls one after another, two links each as in selftest: far more of
     * them than there are slots. */
    for (int call = 1; call <= 50; call++)
    {
        v42_state_t *a = open_link(1);
        v42_state_t *b = open_link(0);

        if (a == NULL || b == NULL)
        {
            printf("FAIL call %d: v42_init failed\n", call);
            return 1;
        }
        snprintf(when, sizeof(when), "call %d, both links up", call);
        check_slots(when, 2);
        if (dm_v42_xid_done(a) || dm_v42_xid_done(b))
        {
            printf("FAIL call %d: XID done before any was exchanged\n", call);
            failures++;
        }
        v42_free(a);
        v42_free(b);
        snprintf(when, sizeof(when), "call %d, both links freed", call);
        check_slots(when, 0);
    }

    /* Two long calls while others come and go beside them. */
    {
        v42_state_t *a = open_link(1);
        v42_state_t *b = open_link(0);

        for (int call = 1; call <= 20; call++)
            v42_free(open_link(call & 1));
        check_slots("two long calls, twenty short ones beside them", 2);
        v42_free(a);
        v42_free(b);
        check_slots("the long calls freed", 0);
    }

    /* A context in the caller's own memory, released and initialised again
     * where it was: the slot left at that address is not inherited. */
    {
        static v42_state_t mem;

        if (v42_init(&mem, 1, 1, frame_get, frame_put, NULL) == NULL)
        {
            printf("FAIL v42_init into the caller's memory\n");
            return 1;
        }
        dm_v42_set_t401(&mem, 1500);
        check_slots("a context in the caller's memory", 1);
        v42_release(&mem);
        v42_init(&mem, 0, 1, frame_get, frame_put, NULL);
        check_slots("initialised again at the same address", 0);
        v42_release(&mem);
    }

    /* A link that is up rides out ten seconds of no answers - a bad patch
     * and the retrain after it - where spandsp's N400 of 5 gave up in about
     * six; and still gives up on a far end that has gone, saying why, not
     * what became of the DISC that followed. */
    {
        char why[96];

        if (!survives_cut(10.0, why, sizeof(why)))
        {
            printf("FAIL a link hung up after ten seconds without answers: %s\n", why);
            failures++;
        }
        if (survives_cut(25.0, why, sizeof(why)))
        {
            printf("FAIL a link waited out twenty-five seconds without answers\n");
            failures++;
        }
        else if (strcmp(why, "the far end stopped answering our polls") != 0)
        {
            printf("FAIL a link that gave up said why as \"%s\"\n", why);
            failures++;
        }
    }

    if (failures == 0)
        printf("v42: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
