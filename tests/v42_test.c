/* Unit tests for the vendored V.42's per-context state (third_party/
 * spandsp-v42, patch 13): a context takes a slot in the side table and gives
 * it back when it is freed, so that the ninth link in a process, and the
 * hundredth, has one of its own - and starts from nothing. */
#include <stdint.h>
#include <stdio.h>

/* For a v42_state_t in our own memory, as the last test needs. */
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

extern void dm_v42_set_t401(const v42_state_t *s, int ms);
extern int dm_v42_xid_done(const v42_state_t *s);
extern int dm_v42_xid_slots_in_use(void);

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

    if (failures == 0)
        printf("v42: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
