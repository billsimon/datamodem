/* Unit tests for the small shared pieces in util.c: caller ID parsing, rate
 * memory and the retrain count. */
#include "datamodem/util.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check_party(const char *in, const char *want_name, const char *want_user)
{
    char name[64], user[64];

    dm_parse_sip_party(in, name, sizeof(name), user, sizeof(user));
    if (strcmp(name, want_name) != 0 || strcmp(user, want_user) != 0)
    {
        printf("FAIL %-55s name=[%s] want [%s], user=[%s] want [%s]\n", in ? in : "(null)", name,
               want_name, user, want_user);
        failures++;
    }
}

int main(void)
{
    /* quoted display names, with escapes */
    check_party("\"Alice Smith\" <sip:+15551234@trunk.example.com;user=phone>", "Alice Smith", "+15551234");
    check_party("\"say \\\"hi\\\"\" <sip:100@pbx>", "say \"hi\"", "100");
    check_party("  \"\" <sip:5550100@host>", "", "5550100");
    /* unquoted display names */
    check_party("Bob <sips:bob@example.com>", "Bob", "bob");
    check_party("Carol  Jones   <sip:200@x>", "Carol  Jones", "200");
    /* bare URIs and no user part */
    check_party("<sip:call@127.0.0.1>", "", "call");
    check_party("sip:5551234@host", "", "5551234");
    check_party("sip:host.example.com", "", "");
    check_party("<sip:host.example.com;transport=tcp>", "", "");
    check_party("sip:host;user=phone", "", "");
    /* tel: URIs have no host */
    check_party("tel:+15551234;phone-context=example.com", "", "+15551234");
    check_party("<tel:911>", "", "911");
    check_party("tel:+15550000", "", "+15550000");
    /* percent-encoding, and control characters dropped */
    check_party("<sip:%2B1555%31@host>", "", "+15551");
    check_party("\"Evil\x1b[2J\" <sip:6%0A6@host>", "Evil[2J", "66");
    /* anonymous, garbage, and nothing at all */
    check_party("\"Anonymous\" <sip:anonymous@anonymous.invalid>", "Anonymous", "anonymous");
    check_party("not a uri", "", "");
    check_party("", "", "");
    check_party(NULL, "", "");

    /* truncation to the space given, always terminated */
    {
        char name[4], user[4];
        dm_parse_sip_party("\"Longname\" <sip:123456789@h>", name, sizeof(name), user, sizeof(user));
        if (strcmp(name, "Lon") != 0 || strcmp(user, "123") != 0)
        {
            printf("FAIL truncation: name=[%s] user=[%s]\n", name, user);
            failures++;
        }
    }

    /* rate memory: five minutes, twenty, then eighty, and never for a rate
     * that ran five minutes before it failed */
    {
        dm_rate_memory_t m = { 0 };
        const long long min = 60LL * 8000;
        double h;

        if (dm_rate_memory_limit(&m, 0) != 0)
        {
            printf("FAIL rate memory: a limit before anything failed\n");
            failures++;
        }
        h = dm_rate_memory_failed(&m, 14400, 9 * min, 10 * min);
        if (h != 300.0 || dm_rate_memory_limit(&m, 10 * min) != 14400 ||
            dm_rate_memory_limit(&m, 15 * min - 1) != 14400 || dm_rate_memory_limit(&m, 15 * min) != 0)
        {
            printf("FAIL rate memory: first failure held %.0f s\n", h);
            failures++;
        }
        /* A higher rate failing does not lift a lower limit still in force;
         * a lower one lowers it. */
        h = dm_rate_memory_failed(&m, 9600, 10 * min, 11 * min);
        if (h != 1200.0 || dm_rate_memory_limit(&m, 11 * min) != 9600)
        {
            printf("FAIL rate memory: second failure held %.0f s\n", h);
            failures++;
        }
        h = dm_rate_memory_failed(&m, 14400, 11 * min, 12 * min);
        if (h != 4800.0 || dm_rate_memory_limit(&m, 12 * min) != 9600 ||
            dm_rate_memory_limit(&m, 92 * min - 1) != 9600 || dm_rate_memory_limit(&m, 92 * min) != 0)
        {
            printf("FAIL rate memory: third failure held %.0f s, limit %d\n", h,
                   dm_rate_memory_limit(&m, 12 * min));
            failures++;
        }
        if (dm_rate_memory_failed(&m, 12000, 100 * min, 101 * min) != 4800.0)
        {
            printf("FAIL rate memory: the hold grew past eighty minutes\n");
            failures++;
        }
    }
    {
        dm_rate_memory_t m = { 0 };
        char text[96];

        /* Five minutes and more at a rate is a rate that held. */
        if (dm_rate_memory_failed(&m, 31200, 0, 6LL * 60 * 8000) != 0.0 ||
            dm_rate_memory_limit(&m, 6LL * 60 * 8000) != 0 || m.count != 0)
        {
            printf("FAIL rate memory: a rate that held six minutes was held against\n");
            failures++;
        }
        dm_rate_memory_text(1200.0, text, sizeof(text));
        if (strcmp(text, "staying below it for 20 minutes") != 0)
        {
            printf("FAIL rate memory text: [%s]\n", text);
            failures++;
        }
    }

    /* a retrain's failure, put back if the round trip moved across it */
    {
        const long long min = 60LL * 8000;
        dm_rate_memory_t rx = { 0 }, tx = { 0 };
        dm_path_watch_t w = { 0 };

        /* 128 to 148 ms, as a MICA's path did: the failure goes. */
        dm_path_watch_arm(&w, 128.0, &rx, &tx);
        dm_rate_memory_failed(&tx, 31200, 0, min);
        if (!dm_path_watch_judge(&w, 148.0, &rx, &tx) || dm_rate_memory_limit(&tx, min) != 0 || tx.count != 0)
        {
            printf("FAIL path watch: a 20 ms move left %d held\n", dm_rate_memory_limit(&tx, min));
            failures++;
        }
        /* Measured again within a few ms: the failure stands. */
        dm_path_watch_arm(&w, 148.0, &rx, &tx);
        dm_rate_memory_failed(&tx, 31200, 0, min);
        if (dm_path_watch_judge(&w, 152.0, &rx, &tx) || dm_rate_memory_limit(&tx, min) != 31200)
        {
            printf("FAIL path watch: a 4 ms move put the failure back\n");
            failures++;
        }
        /* Judged once only, and not without a measurement each side. */
        if (dm_path_watch_judge(&w, 300.0, &rx, &tx) || dm_rate_memory_limit(&tx, min) != 31200)
        {
            printf("FAIL path watch: judged a second time\n");
            failures++;
        }
        dm_path_watch_arm(&w, -1.0, &rx, &tx);
        dm_rate_memory_failed(&rx, 28800, 0, min);
        if (dm_path_watch_judge(&w, 300.0, &rx, &tx) || dm_rate_memory_limit(&rx, min) != 28800)
        {
            printf("FAIL path watch: judged with no round trip before\n");
            failures++;
        }
        /* One memory, as V.32 bis has; and a move with nothing to put back. */
        {
            dm_rate_memory_t m = { 0 };

            dm_path_watch_arm(&w, 200.0, &m, NULL);
            if (dm_path_watch_judge(&w, 240.0, &m, NULL))
            {
                printf("FAIL path watch: said it put back a failure there was none of\n");
                failures++;
            }
            dm_path_watch_arm(&w, 200.0, &m, NULL);
            dm_rate_memory_failed(&m, 14400, 0, min);
            if (!dm_path_watch_judge(&w, 180.0, &m, NULL) || dm_rate_memory_limit(&m, min) != 0)
            {
                printf("FAIL path watch: a round trip 20 ms shorter left the failure held\n");
                failures++;
            }
        }
    }

    /* retrain counting for --max-retrains */
    {
        dm_retrain_count_t c = { 0 };
        const int64_t s = 1000;

        /* V.34, 31200 in and 28800 out; the far end retrains three times
         * and each time our transmitter comes back slower: none count. */
        dm_retrain_count_update(&c, 0, 31200, 28800, true, 10 * s);
        dm_retrain_count_update(&c, 0, 31200, 28800, false, 20 * s);
        if (dm_retrain_count_update(&c, 1, 31200, 26400, true, 25 * s) != 0 ||
            dm_retrain_count_update(&c, 2, 31200, 24000, true, 45 * s) != 0)
        {
            printf("FAIL retrain count: retrains that came back slower counted\n");
            failures++;
        }
        /* The same rate again counts, and so does a faster one. */
        if (dm_retrain_count_update(&c, 3, 31200, 24000, true, 60 * s) != 1 ||
            dm_retrain_count_update(&c, 4, 33600, 24000, true, 70 * s) != 2)
        {
            printf("FAIL retrain count: %u strikes for retrains that did not slow down\n", c.strikes);
            failures++;
        }
        /* Two at once, seen in one look. */
        if (dm_retrain_count_update(&c, 6, 33600, 24000, true, 80 * s) != 4)
        {
            printf("FAIL retrain count: %u strikes after two retrains in one look\n", c.strikes);
            failures++;
        }
        /* Two quiet minutes, and it starts again. */
        if (dm_retrain_count_update(&c, 7, 33600, 24000, true, 80 * s + 121 * s) != 1)
        {
            printf("FAIL retrain count: %u strikes after two quiet minutes\n", c.strikes);
            failures++;
        }
    }
    {
        dm_retrain_count_t c = { 0 };
        const int64_t m = 60 * 1000;

        /* The V.32 bis call that prompted two minutes: clean retrains at
         * 9600 every three or four minutes count for nothing. */
        dm_retrain_count_update(&c, 0, 9600, 0, true, 0);
        for (unsigned k = 1; k <= 8; k++)
            dm_retrain_count_update(&c, k, 9600, 0, true, (int64_t) k * 3 * m + 30 * 1000);
        if (c.strikes > 1)
        {
            printf("FAIL retrain count: %u strikes for retrains three minutes apart\n", c.strikes);
            failures++;
        }
    }
    {
        dm_retrain_count_t c = { 0 };

        /* V.22bis, one rate both ways: settling lower is the cure. */
        dm_retrain_count_update(&c, 0, 2400, 0, true, 1000);
        if (dm_retrain_count_update(&c, 1, 1200, 0, true, 2000) != 0 ||
            dm_retrain_count_update(&c, 2, 1200, 0, true, 3000) != 1)
        {
            printf("FAIL retrain count: V.22bis, %u strikes\n", c.strikes);
            failures++;
        }
    }

    if (failures == 0)
        printf("util: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
