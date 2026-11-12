/* Unit tests for the small shared pieces in util.c: caller ID parsing, rate
 * memory, the retrain count and RTP arrival. */
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

/* RTP arrival: what the modem is given, sample by sample - the sequence
 * number of the packet each came from, which the test packets carry as
 * their samples, or -1 for nothing. */
#define RX_MAX 16000
static int rx_got[RX_MAX];
static int rx_n;

static void rx_out(void *user, const int16_t *s, int n)
{
    (void) user;
    for (int i = 0; i < n && rx_n < RX_MAX; i++)
        rx_got[rx_n++] = s ? s[i] : -1;
}

static void rx_push_ts(dm_rtp_rx_t *q, uint16_t seq, uint32_t ts)
{
    int16_t s[160];

    for (int i = 0; i < 160; i++)
        s[i] = (int16_t) (seq & 0x7fff);
    dm_rtp_rx_push(q, 1234, seq, ts, s, 160);
}

static void rx_push(dm_rtp_rx_t *q, uint16_t seq)
{
    rx_push_ts(q, seq, 1000u + 160u * seq);
}

/* want_n samples, want_none of them nothing, and the packets in order with
 * none repeated: each packet's samples together, each after the last. */
static void rx_check(const char *what, int want_n, int want_none)
{
    int none = 0, last = -1, run = 0;

    for (int i = 0; i < rx_n; i++)
    {
        if (rx_got[i] < 0)
        {
            none++;
            continue;
        }
        if (rx_got[i] != last)
        {
            int ahead = (rx_got[i] - last) & 0x7fff; /* the samples carry 15 bits of it */

            if (last >= 0 && (ahead == 0 || ahead >= 0x4000))
            {
                printf("FAIL rtp rx %s: packet %d after %d\n", what, rx_got[i], last);
                failures++;
                return;
            }
            last = rx_got[i];
            run = 0;
        }
        if (++run > 160)
        {
            printf("FAIL rtp rx %s: packet %d given twice\n", what, last);
            failures++;
            return;
        }
    }
    if (rx_n != want_n || none != want_none)
    {
        printf("FAIL rtp rx %s: %d samples (%d of nothing), want %d (%d)\n", what, rx_n, none, want_n, want_none);
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

    /* a far-end retrain's failure, put back if it lost our packets around it */
    {
        const long long sec = 8000, min = 60 * sec;
        dm_loss_watch_t w = { 0 };
        dm_rate_memory_t m = { 0 };

        /* The report after the retrain, as the MICA's came: 2.4 s. */
        dm_loss_watch_arm(&w, 100 * sec, &m);
        dm_rate_memory_failed(&m, 31200, 0, 100 * sec);
        if (dm_loss_watch_check(&w, 100 * sec, &m) || !dm_loss_watch_loss(&w, 102 * sec + sec / 2, &m) ||
            dm_rate_memory_limit(&m, 103 * sec) != 0)
        {
            printf("FAIL loss watch: a loss 2.5 s after the retrain left the failure held\n");
            failures++;
        }
        /* Before it: 2 s. */
        dm_loss_watch_loss(&w, 198 * sec, &m);
        dm_loss_watch_arm(&w, 200 * sec, &m);
        dm_rate_memory_failed(&m, 31200, 0, 200 * sec);
        if (!dm_loss_watch_check(&w, 200 * sec, &m) || dm_rate_memory_limit(&m, 200 * sec) != 0)
        {
            printf("FAIL loss watch: a loss 2 s before the retrain left the failure held\n");
            failures++;
        }
        /* Too far either side: the failure stands. */
        dm_loss_watch_loss(&w, 290 * sec, &m);
        dm_loss_watch_arm(&w, 300 * sec, &m);
        dm_rate_memory_failed(&m, 28800, 0, 300 * sec);
        if (dm_loss_watch_check(&w, 300 * sec, &m) || dm_loss_watch_loss(&w, 309 * sec, &m) ||
            dm_rate_memory_limit(&m, 309 * sec) != 28800)
        {
            printf("FAIL loss watch: a loss ten seconds off put the failure back\n");
            failures++;
        }
        /* And only once for each retrain. */
        if (dm_loss_watch_loss(&w, 310 * sec, &m) || dm_rate_memory_limit(&m, min * 6) != 28800)
        {
            printf("FAIL loss watch: one retrain forgiven twice\n");
            failures++;
        }
    }

    /* a training signal's error, leaving a burst of noise out */
    {
        double steady[8] = { 1.0, 1.2, 0.9, 1.1, 1.0, 0.8, 1.3, 1.0 };
        double burst[8] = { 1.0, 1.2, 500.0, 800.0, 1.0, 0.8, 1.3, 1.0 };
        double bad[8] = { 500.0, 600.0, 500.0, 800.0, 400.0, 700.0, 1.0, 500.0 };
        double m;

        m = dm_robust_mean(steady, 8);
        if (m < 1.03 || m > 1.04)
        {
            printf("FAIL robust mean: a steady line came to %.3f, not its mean\n", m);
            failures++;
        }
        m = dm_robust_mean(burst, 8);
        if (m < 1.04 || m > 1.06)
        {
            printf("FAIL robust mean: a burst was not left out (%.3f)\n", m);
            failures++;
        }
        /* A line bad throughout is bad, whatever one block says. */
        m = dm_robust_mean(bad, 8);
        if (m < 400.0)
        {
            printf("FAIL robust mean: a bad line came to %.3f\n", m);
            failures++;
        }
        if (dm_robust_mean(steady, 0) != 0.0)
        {
            printf("FAIL robust mean: no blocks\n");
            failures++;
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

    {
        static dm_rtp_rx_t q;
        const uint16_t s0 = 65530; /* across the wrap */

        /* In order, a packet a tick: each goes as it comes. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        for (int k = 0; k < 10; k++)
        {
            rx_push(&q, (uint16_t) (s0 + k));
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        }
        rx_check("in order", 1600, 0);

        /* Two at once - the far end's clock ahead of ours, or a packet more
         * on the path - both go at once, and nothing is kept back. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        rx_push(&q, (uint16_t) (s0 + 1));
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("two at once", 320, 0);

        /* Out of order: the third waits for the second. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_push(&q, (uint16_t) (s0 + 2));
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_push(&q, (uint16_t) (s0 + 1));
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("out of order", 480, 0);

        /* Lost: after the hold, nothing in its place, and then what came
         * after it. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        for (int k = 2; k < 6; k++)
        {
            rx_push(&q, (uint16_t) (s0 + k));
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        }
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("lost", 960, 160);
        if (q.lost != 1 || q.queued != 0)
        {
            printf("FAIL rtp rx lost: %u lost, %d waiting\n", q.lost, q.queued);
            failures++;
        }

        /* Nothing at all for a while, then everything at once, late: the
         * modem's clock runs on through it, and what comes late is skipped
         * as far as the modem was given nothing for it. Nothing after it is
         * delayed. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        for (int k = 0; k < 5; k++)
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        for (int k = 1; k < 7; k++)
            rx_push(&q, (uint16_t) (s0 + k));
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("stall", 7 * 160, 3 * 160);
        if (q.late != 3 || q.queued != 0 || q.owed != 0)
        {
            printf("FAIL rtp rx stall: %u late, %d waiting, %d owed\n", q.late, q.queued, q.owed);
            failures++;
        }

        /* A path that has become slower for good: after the stall, a
         * packet a tick as before, every one of them later than it was.
         * The production failure: from there on every packet was late, and
         * the modem was given nothing for the rest of the call. Now it
         * costs one gap, and then each packet goes as it comes. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        for (int k = 0; k < 5; k++)
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        for (int k = 1; k < 30; k++)
        {
            rx_push(&q, (uint16_t) (s0 + k));
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        }
        rx_check("slower path", 160 + 3 * 160 + 26 * 160, 3 * 160);
        if (q.late != 3 || q.queued != 0)
        {
            printf("FAIL rtp rx slower path: %u late, %d waiting\n", q.late, q.queued);
            failures++;
        }

        /* Lost in a stall: what the modem was given nothing for during it
         * pays for the packets that never came, and is not given again. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        for (int k = 0; k < 5; k++)
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_push(&q, (uint16_t) (s0 + 6));
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("lost in a stall", 160 + 5 * 160 + 160, 5 * 160);
        if (q.lost != 5 || q.owed != 0)
        {
            printf("FAIL rtp rx lost in a stall: %u lost, %d owed\n", q.lost, q.owed);
            failures++;
        }

        /* Timestamps that jump, the sequence numbers running on: back, or
         * far ahead, counted and nothing more; a packet's worth ahead - a
         * carrier taking 20 ms out and numbering on - and that much nothing
         * goes in the gap, as for a packet lost. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push_ts(&q, s0, 1000);
        rx_push_ts(&q, (uint16_t) (s0 + 1), 1160);
        rx_push_ts(&q, (uint16_t) (s0 + 2), 900);   /* back */
        rx_push_ts(&q, (uint16_t) (s0 + 3), 50000); /* a new timeline */
        rx_push_ts(&q, (uint16_t) (s0 + 4), 50320); /* 20 ms taken out */
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("timestamps jump", 6 * 160, 160);
        if (q.ts_jumps != 3 || q.ts_skipped != 160)
        {
            printf("FAIL rtp rx timestamps jump: %u jumps counted, %llu skipped\n", q.ts_jumps, q.ts_skipped);
            failures++;
        }

        /* Lost, with the timestamps saying more went missing than the
         * packets: the lot is given as nothing. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push_ts(&q, s0, 1000);
        rx_push_ts(&q, (uint16_t) (s0 + 2), 1000 + 4 * 160);
        for (int k = 0; k < 4; k++)
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        rx_check("lost and taken out", 2 * 160 + 3 * 160, 3 * 160);

        /* A new SSRC, or sequence numbers that leap, start again. */
        dm_rtp_rx_reset(&q);
        rx_n = 0;
        rx_push(&q, s0);
        dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        {
            int16_t s[160];

            for (int i = 0; i < 160; i++)
                s[i] = 500;
            dm_rtp_rx_push(&q, 99, 500, 0, s, 160);
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
            for (int i = 0; i < 160; i++)
                s[i] = 9000;
            dm_rtp_rx_push(&q, 99, 9000, 0, s, 160);
            dm_rtp_rx_drain(&q, 160, 320, rx_out, NULL);
        }
        rx_check("new stream", 480, 0);
        if (q.resyncs != 2)
        {
            printf("FAIL rtp rx new stream: %u resyncs\n", q.resyncs);
            failures++;
        }
    }

    if (failures == 0)
        printf("util: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
