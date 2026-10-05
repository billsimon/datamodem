#include "datamodem/modem.h"
#include "datamodem/log.h"
#include "datamodem/ring.h"
#include "datamodem/util.h"
#include "datamodem/v32.h"
#include "datamodem/v34.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* V.42's transmit bit rate has to be corrected after init and there is no
 * API for it - see v42_set_bit_rate(). That needs the private structure. */
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

/* The transmit queue holds this many seconds of line time. It is deliberately
 * short: anything sitting in it is already committed, so a big queue only
 * means a longer wait between changing your mind and the line noticing. At
 * 300 bps four seconds is about 120 characters. The receive queue is fixed
 * and large, because nothing downstream of it is slow - it only has to cover
 * the stretch where command mode holds the screen. */
#define DM_TX_SECONDS 4
#define DM_TX_QUEUE_MIN 256
#define DM_RX_QUEUE 65536

/* Under V.42 the receive queue need never overflow: when it is this close to
 * full, LAPM tells the far end to wait (RNR), and it is let go again once
 * there is this much room. The gap stops it flapping; what arrives meanwhile
 * - a decompressed frame at most - is far less than either. Without V.42
 * there is no way to ask, and the oldest bytes go. */
#define DM_RX_BUSY_FREE 16384
#define DM_RX_READY_FREE 32768

#define DM_SAMPLE_RATE 8000

/* A dropped carrier is not automatically the end of the call: real lines
 * glitch, and a V.22bis pair will retrain in under two seconds when they do.
 * Only a carrier that stays away this long counts as NO CARRIER. Longer than
 * a retrain takes, shorter than anyone's patience.
 *
 * V.32 gets longer. Its retrain is the whole start-up procedure again - the
 * tone exchange, two training sequences each sized to the round trip, and
 * the rate exchange - which is about four seconds on a 200 ms path and more
 * on a longer one. */
#define DM_CARRIER_GRACE_MS 4000
#define DM_CARRIER_GRACE_V32_MS 12000
/* V.34's retrain goes back through Phase 2 - tones, probing, two training
 * sequences sized to the round trip, and the MP exchange - which is longer
 * again. */
#define DM_CARRIER_GRACE_V34_MS 15000

/* Staging for compressor output waiting to be packed into LAPM frames.
 * Generous: V.42bis in transparent mode can expand slightly, and a frame is
 * only ever 128 bytes, so this is many frames' worth of headroom. */
#define DM_COMP_QUEUE 8192
/* How much plaintext to push through the compressor in one go, and the free
 * space required in the staging queue before doing so. The gap between them
 * is the worst-case expansion, with a lot to spare. */
#define DM_COMP_CHUNK 512
#define DM_COMP_HEADROOM 2048

/* While V.42 detection is still undecided the far end may already be
 * talking - a host that does not do V.42 starts sending its banner the
 * moment it has a carrier. Those bits go to the detector, so unless they
 * are framed in parallel and held, everything said before we give up on
 * V.42 is lost. Ten seconds at 2400 bps is 3 kB; this is comfortably more,
 * and it fills from the front, so what survives is the beginning of the
 * banner rather than the end. */
#define DM_PENDING_RX 8192

/* Handing a byte to the link layer is not the same as putting it on the
 * line. With V.42 especially, LAPM swallows our whole queue into its window
 * in an instant and then spends the next half minute actually transmitting
 * it - so "our queue is empty" is not remotely the same question as "is it
 * safe to hang up now".
 *
 * spandsp can be asked how many frames LAPM still holds, but the answer is
 * useless for this: its acknowledgement bookkeeping lags the real delivery
 * by as much as a minute and a half, so waiting on it would keep a finished
 * call up long after the data had arrived.
 *
 * So the drain is estimated from the line rate instead, which is the thing
 * that actually governs it. Ten bits per byte covers start/stop framing and
 * HDLC overhead alike, and the tail covers packetisation and the far end's
 * jitter buffer - the last RTP packet has to be played out over there before
 * we clear the call. */
#define DM_LINE_BITS_PER_BYTE 10
#define DM_LINE_TAIL_MS 750

/* V.42's N401 - the most data one LAPM I-frame may carry. spandsp defaults
 * to this and caps it here too, but only says so in its private header, so
 * the number is repeated rather than reached for. The compressor is told not
 * to emit blocks bigger than this, so everything it produces fits a frame. */
#define DM_LAPM_MAX_FRAME 128

/* spandsp reports this through the V.42 status callback when its detection
 * phase gives up on the far end. The value is real API - lapm_status_to_str()
 * is exported and will name it - but the enum itself is file-local to
 * spandsp's v42.c and appears in no header, so the number is repeated here
 * and checked against the library at startup. */
#define DM_LAPM_V42_UNSUPPORTED 8

/* V.42's detection phase recognises a repeating pattern, so what each end
 * needs is a number of bit periods, not a number of milliseconds. spandsp's
 * detector wants about 512 of them; this is double, for margin. */
#define DM_V42_DETECT_MIN_BITS 1024

#if defined(DATAMODEM_VENDORED_V42)
/* See third_party/spandsp-v42. */
extern char dm_v42_disconnect_cause[80];
extern int dm_v42_reconnect(v42_state_t *s);
extern int dm_v42_refuse_sabme;
extern unsigned dm_v42_fcs_errors;
extern int dm_v42_adp_no_ec;
extern int dm_v42_answer_no_ec;
extern int dm_v42_no_xid;
extern int dm_v42_xid_done(const v42_state_t *s);
extern void dm_v42_set_t401(const v42_state_t *s, int ms);
#endif

/* How long to leave between attempts at establishment. Long enough that a
 * far end which said "not yet" has had a chance to become ready, short
 * enough to fit several tries inside a default --v42-timeout. */
#define DM_V42_RETRY_MS 1500

/* How many times to re-offer establishment to a far end that answers with a
 * point-blank DM. V.42 8.3.2.1 makes a DM response a legitimate "I cannot
 * enter the connected state", and a far end that says it once may well be
 * ready a moment later - but one that says it every time has made up its
 * mind, and there is no sense spending the whole of --v42-timeout finding
 * that out. Silence gets the full timeout, because silence may be a path
 * losing frames rather than an answer. */
#define DM_V42_MAX_REFUSALS 2

/* How long to watch an under-offer link before judging whether the two ends
 * actually agreed about the rate, and how bad the framing has to be to say
 * they did not. A link that is demodulating the far end at the wrong rate is
 * reading noise, so roughly half of its characters arrive with the stop bit
 * in the wrong place; one that is merely on a poor line is nothing like
 * that. */
#define DM_RATE_CHECK_MS 2500
#define DM_RATE_CHECK_MIN_CHARS 40

/* T400, the detection window, is 750 ms in the specification - which assumes
 * a copper pair where the far end's reply comes back more or less at once.
 * Over RTP it does not: there is a jitter buffer at each end, and the whole
 * exchange has to fit inside the window:
 *
 *     far end recognises our ODP     ~DM_V42_DETECT_MIN_BITS
 *   + its reply crosses the path     ~2 x jitter buffer
 *   + we recognise its ADP           ~DM_V42_DETECT_MIN_BITS
 *
 * At 2400 bps with the default 150 ms buffers that is about 1.15 s, so the
 * specified 750 ms expires while the far end's ADP is still in flight. We
 * then declare it non-V.42 and fall back - and it, having committed to
 * LAPM, carries on sending HDLC that we read as line noise until one side
 * gives up. Budget the window for the path it is actually running over.
 *
 * Erring long is cheap here: T400 governs only how long we keep listening
 * before giving up, it never makes the far end wait, and anything the far
 * end says meanwhile is held and delivered on fallback rather than lost. */
#define DM_V42_DETECT_SPEC_MS 750

typedef enum
{
    DM_MOD_V22BIS = 0,
    DM_MOD_V22,
    DM_MOD_V21,
    DM_MOD_BELL103,
    DM_MOD_V23,
    DM_MOD_V32,
    DM_MOD_V32BIS,
    DM_MOD_V34
} dm_mod_t;

typedef enum
{
    DM_V42_OFF = 0,
    DM_V42_DETECT,   /* try, and fall back to raw async if the far end does not */
    DM_V42_REQUIRE   /* try, and drop the call if the far end does not */
} dm_v42_mode_t;

struct dm_modem
{
    pthread_mutex_t lock;

    /* Settings, fixed after create. */
    dm_mod_t mod;
    const char *mod_name;
    bool calling;
    int offered_rate;
    int tx_bit_rate;         /* what we transmit at; V.23 differs by direction */
    int carrier_grace_ms;
    int guard;
    int data_bits;
    int parity;              /* ASYNC_PARITY_* */
    int stop_bits;
    bool v14;
    float tx_power;
    int path_delay_ms;
    char tag[64];

    /* Pre-carrier phase. */
    int answer_tone_samples;  /* answerer: ANS burst length, 0 = none */
    int wait_samples;         /* caller: samples left to listen for ANS */
    int ans_tail_samples;     /* caller: samples left to let the tone finish */
    bool ans_tail_running;
    modem_connect_tones_tx_state_t *tone_tx;
    modem_connect_tones_rx_state_t *tone_rx;

    /* Data pumps. Exactly one family is live. */
    dm_v32_t *v32;
    dm_v34_t *v34;
    v22bis_state_t *v22;
    fsk_tx_state_t *fsk_tx;
    fsk_rx_state_t *fsk_rx;
    async_rx_state_t *arx;
    /* A second framer, identical except that V.14 is off, whose only job is
     * to count stop bits that landed in the wrong place. V.14 rate adaption
     * exists precisely to tolerate that, so the real framer - which has it
     * on by default - reports no framing errors at all and cannot be asked
     * whether what it is framing is data or noise. See
     * check_rate_agreement(). */
    async_rx_state_t *arx_monitor;

    /* Error correction and compression. When v42 is running it replaces the
     * asynchronous character framing entirely: the pump's bits are LAPM's,
     * not a start/stop stream. */
    dm_v42_mode_t v42_mode;
    bool v42_peer_declined;    /* spandsp's detection gave up on the far end */
    v42_state_t *v42;
    v42bis_state_t *v42bis;
    /* What the XID exchange agreed, which is what V.42bis actually runs
     * with - possibly less than was offered, possibly one direction only,
     * possibly nothing. Until the link is up, the offer. */
    bool comp_tx;
    bool comp_rx;
    /* The far end never answered our XID, so nothing was agreed - but some
     * far ends compress anyway. Until it shows its hand, received data goes
     * to the terminal as it is and through the decompressor too, its output
     * thrown away, so that the dictionary is in step if the far end turns
     * out to be compressing. See v42bis_shadow(). */
    bool rx_shadow;
    bool rx_shadow_esc;       /* a 0x00 is held: the next octet decides */
    bool rx_discard;          /* the decompressor's output is not wanted */
    int v42bis_dict;
    int v42bis_max_string;
    bool lapm_up;
    bool rx_busy;              /* LAPM told the far end to wait: our receive queue is full */
    bool v42_fell_back;        /* gave up on V.42 and went back to raw async */
    int v42_timeout_s;
    int64_t v42_deadline_ms;   /* 0 until the carrier is up */
    int64_t v42_retry_at_ms;   /* 0 = no establishment retry pending */
    unsigned v42_establish_tries;
    unsigned v42_refusals;     /* DM answers to our SABME */
    bool v42_establish_failed; /* the far end does V.42; we could not agree */
    bool v42_adp_declined;     /* its detection pattern asked for no error correction */
    int drop_ceiling_to;       /* >0: re-offer this rate at the next safe moment */
    bool rate_pinned;          /* the ceiling has been dropped once already */
    bool rate_proven;          /* and the framing came good afterwards */
    bool rate_complained;      /* and it did not, and we have said so once */
    int rate_bad_windows;      /* consecutive windows of bad framing */
    int64_t rate_check_ms;     /* when the current rate-agreement window opened */
    int rate_check_errors;     /* framing errors at the start of it */
    uint64_t rate_check_bytes;
    unsigned frame_errors;
    dm_ring_t pending_rx;      /* framed while V.42 is still undecided */
    uint64_t wire_tx;
    uint64_t wire_rx;
    dm_ring_t comp_out;        /* compressor output waiting for a frame */

    /* Our own asynchronous transmit framer, see tx_get_bit(). */
    int tx_bitpos;
    unsigned tx_byte;
    int tx_parity;

    /* State. */
    dm_modem_phase_t phase;
    bool armed;              /* the call is up; the clock may run */
    bool pump_started;
    bool connected;
    bool carrier_lost;
    bool answer_tone_seen;
    int bit_rate;
    unsigned retrains;
    int64_t connect_ms;
    /* DATAMODEM_V32_RENEGOTIATE, a test hook: ask for this rate this long
     * after the carrier came up. */
    int reneg_after_ms;
    int reneg_rate;
    int64_t carrier_down_ms;  /* 0 unless the carrier is currently missing */
    int64_t line_busy_until_ms; /* estimated: when the line finishes what it has */
    int64_t last_rx_ms;
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    bool wake_pending;

    dm_ring_t tx;
    dm_ring_t rx;
    void (*wake)(void *user);
    void *wake_user;
};

static int g_spandsp_level = SPAN_LOG_WARNING;

/* ------------------------------------------------------------- plumbing */

void dm_modem_init_logging(const dm_config_t *cfg)
{
    if (cfg->spandsp_log_level >= 0)
    {
        g_spandsp_level = cfg->spandsp_log_level;
    }
    else
    {
        switch (cfg->log_level)
        {
        case DM_LOG_ERROR:
            g_spandsp_level = SPAN_LOG_ERROR;
            break;
        case DM_LOG_WARN:
            g_spandsp_level = SPAN_LOG_WARNING;
            break;
        case DM_LOG_INFO:
            g_spandsp_level = SPAN_LOG_PROTOCOL_WARNING;
            break;
        case DM_LOG_DEBUG:
            g_spandsp_level = SPAN_LOG_FLOW_2;
            break;
        default:
            g_spandsp_level = SPAN_LOG_DEBUG_2;
            break;
        }
    }
    span_set_message_handler(dm_log_spandsp_message);
    span_set_error_handler(dm_log_spandsp_error);

    /* DM_LAPM_V42_UNSUPPORTED is copied out of spandsp's internals. If a
     * future spandsp renumbers it, say so rather than silently stop
     * noticing that the far end declined V.42. */
    {
        const char *name = lapm_status_to_str(DM_LAPM_V42_UNSUPPORTED);

        if (name == NULL || strstr(name, "UNSUPPORTED") == NULL)
            DM_WARN("modem", "this spandsp reports status %d as \"%s\", not V.42-unsupported; "
                             "falling back on the --v42-timeout backstop instead",
                    DM_LAPM_V42_UNSUPPORTED, name ? name : "?");
    }
}

static void attach_logging(logging_state_t *lg, const char *tag)
{
    if (lg == NULL)
        return;
    span_log_set_message_handler(lg, dm_log_spandsp_message);
    span_log_set_error_handler(lg, dm_log_spandsp_error);
    span_log_set_level(lg, SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_SHOW_TAG |
                               g_spandsp_level);
    if (tag != NULL && *tag != '\0')
        span_log_set_tag(lg, tag);
}

/* --------------------------------------------------------- option tables */

static bool parse_modulation(const char *s, dm_mod_t *out)
{
    if (s == NULL || *s == '\0' || strcasecmp(s, "v22bis") == 0 || strcasecmp(s, "v.22bis") == 0)
        *out = DM_MOD_V22BIS;
    else if (strcasecmp(s, "v22") == 0 || strcasecmp(s, "v.22") == 0)
        *out = DM_MOD_V22;
    else if (strcasecmp(s, "v21") == 0 || strcasecmp(s, "v.21") == 0)
        *out = DM_MOD_V21;
    else if (strcasecmp(s, "bell103") == 0 || strcasecmp(s, "bell-103") == 0)
        *out = DM_MOD_BELL103;
    else if (strcasecmp(s, "v23") == 0 || strcasecmp(s, "v.23") == 0)
        *out = DM_MOD_V23;
    else if (strcasecmp(s, "v32") == 0 || strcasecmp(s, "v.32") == 0)
        *out = DM_MOD_V32;
    else if (strcasecmp(s, "v32bis") == 0 || strcasecmp(s, "v.32bis") == 0)
        *out = DM_MOD_V32BIS;
    else if (strcasecmp(s, "v34") == 0 || strcasecmp(s, "v.34") == 0)
        *out = DM_MOD_V34;
    else
        return false;
    return true;
}

static const char *modulation_name(dm_mod_t m)
{
    switch (m)
    {
    case DM_MOD_V22BIS:
        return "v22bis";
    case DM_MOD_V22:
        return "v22";
    case DM_MOD_V21:
        return "v21";
    case DM_MOD_BELL103:
        return "bell103";
    case DM_MOD_V23:
        return "v23";
    case DM_MOD_V32:
        return "v32";
    case DM_MOD_V32BIS:
        return "v32bis";
    case DM_MOD_V34:
        return "v34";
    }
    return "?";
}

/* V.32 bis is V.32's pump with more rates; everything around it is the same. */
static bool is_v32(dm_mod_t m)
{
    return m == DM_MOD_V32 || m == DM_MOD_V32BIS;
}

static bool is_fsk(dm_mod_t m)
{
    return m == DM_MOD_V21 || m == DM_MOD_BELL103 || m == DM_MOD_V23;
}

static bool parse_parity(const char *s, int *out)
{
    if (s == NULL || *s == '\0' || strcasecmp(s, "none") == 0 || strcasecmp(s, "n") == 0)
        *out = ASYNC_PARITY_NONE;
    else if (strcasecmp(s, "even") == 0 || strcasecmp(s, "e") == 0)
        *out = ASYNC_PARITY_EVEN;
    else if (strcasecmp(s, "odd") == 0 || strcasecmp(s, "o") == 0)
        *out = ASYNC_PARITY_ODD;
    else
        return false;
    return true;
}

static bool parse_v42_mode(const char *s, dm_v42_mode_t *out)
{
    if (s == NULL || *s == '\0' || strcasecmp(s, "off") == 0 || strcasecmp(s, "none") == 0 ||
        strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0)
        *out = DM_V42_OFF;
    else if (strcasecmp(s, "detect") == 0 || strcasecmp(s, "auto") == 0 || strcasecmp(s, "on") == 0 ||
             strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0)
        *out = DM_V42_DETECT;
    else if (strcasecmp(s, "require") == 0 || strcasecmp(s, "required") == 0)
        *out = DM_V42_REQUIRE;
    else
        return false;
    return true;
}

static bool parse_guard(const char *s, int *out)
{
    if (s == NULL || *s == '\0' || strcasecmp(s, "none") == 0 || strcmp(s, "0") == 0)
        *out = V22BIS_GUARD_TONE_NONE;
    else if (strcmp(s, "550") == 0)
        *out = V22BIS_GUARD_TONE_550HZ;
    else if (strcmp(s, "1800") == 0)
        *out = V22BIS_GUARD_TONE_1800HZ;
    else
        return false;
    return true;
}

/* V.21, Bell 103 and V.23 all split the line into a channel per direction,
 * and which channel is yours depends on who dialled. V.23 is the odd one:
 * its channels are not the same speed, so the calling terminal transmits at
 * 75 bps and receives at 1200 - which is exactly right for videotex, and
 * useless for anything symmetric. */
static const fsk_spec_t *fsk_spec_for(dm_mod_t mod, bool calling, bool transmit)
{
    bool ours = (calling == transmit); /* our tx channel when calling, our rx when answering */

    switch (mod)
    {
    case DM_MOD_V21:
        /* V.21 channel 1 (980/1180 Hz) belongs to the calling modem. */
        return &preset_fsk_specs[ours ? FSK_V21CH1 : FSK_V21CH2];
    case DM_MOD_BELL103:
        /* Bell 103 numbers its channels the other way round from V.21, and
         * so does spandsp: its "Bell103 ch 1" is the ANSWERING band
         * (2025/2225 Hz) and "ch 2" is the originating one (1070/1270 Hz).
         * Getting this backwards is invisible in any test where both ends
         * are this program - both sides invert together and talk happily -
         * and produces two modems shouting into each other's bands the
         * first time one of them meets a real Bell 103. */
        return &preset_fsk_specs[ours ? FSK_BELL103CH2 : FSK_BELL103CH1];
    case DM_MOD_V23:
        /* Channel 1 is the 1200 bps forward channel, from the answering end. */
        return &preset_fsk_specs[ours ? FSK_V23CH2 : FSK_V23CH1];
    default:
        return NULL;
    }
}

static int fsk_rate_for(dm_mod_t mod, bool calling, bool transmit)
{
    if (mod != DM_MOD_V23)
        return 300;
    return (calling == transmit) ? 75 : 1200;
}

static int ans_tone_type(dm_mod_t mod)
{
    /* A Bell 103 answering modem has no separate answer tone: its 2225 Hz
     * mark carrier is the answer tone. */
    return (mod == DM_MOD_BELL103) ? MODEM_CONNECT_TONES_BELL_ANS : MODEM_CONNECT_TONES_ANS;
}

/* What --bit-rate 0, the default, means for each modulation: the most it
 * can do. */
static int default_rate(dm_mod_t mod)
{
    switch (mod)
    {
    case DM_MOD_V34:
        return 33600;
    case DM_MOD_V32BIS:
        return 14400;
    case DM_MOD_V32:
        return 9600;
    case DM_MOD_V22BIS:
        return 2400;
    case DM_MOD_V22:
        return 1200;
    default:
        return 300;
    }
}

bool dm_modem_params_check(const dm_modem_params_t *p, char *err, size_t err_len)
{
    dm_mod_t mod;
    int tmp;

    if (!parse_modulation(p->modulation, &mod))
    {
        snprintf(err, err_len, "unknown modulation '%s'; use v34, v32bis, v32, v22bis, v22, v21, bell103 or v23",
                 p->modulation ? p->modulation : "");
        return false;
    }
    if (!parse_parity(p->parity, &tmp))
    {
        snprintf(err, err_len, "parity must be none, even or odd");
        return false;
    }
    if (!parse_guard(p->guard_tone, &tmp))
    {
        snprintf(err, err_len, "guard tone must be none, 550 or 1800");
        return false;
    }
    if (mod == DM_MOD_V22BIS && p->bit_rate != 0 && p->bit_rate != 1200 && p->bit_rate != 2400)
    {
        snprintf(err, err_len, "V.22bis runs at 1200 or 2400 bps, not %d", p->bit_rate);
        return false;
    }
    if (mod == DM_MOD_V32 && p->bit_rate != 0 && p->bit_rate != 4800 && p->bit_rate != 9600)
    {
        snprintf(err, err_len, "V.32 runs at 9600 or 4800 bps, not %d (2400 is left for further "
                               "study by the Recommendation)", p->bit_rate);
        return false;
    }
    if (mod == DM_MOD_V32BIS && p->bit_rate != 0 && p->bit_rate != 4800 && p->bit_rate != 7200 &&
        p->bit_rate != 9600 && p->bit_rate != 12000 && p->bit_rate != 14400)
    {
        snprintf(err, err_len, "V.32bis runs at 14400, 12000, 9600, 7200 or 4800 bps, not %d", p->bit_rate);
        return false;
    }
    if (mod == DM_MOD_V34 && p->bit_rate != 0 && (p->bit_rate < 2400 || p->bit_rate > 33600 || p->bit_rate % 2400))
    {
        snprintf(err, err_len, "V.34 runs at multiples of 2400 bps from 2400 to 33600, not %d", p->bit_rate);
        return false;
    }
    if (p->data_bits < 5 || p->data_bits > 8)
    {
        snprintf(err, err_len, "data bits must be 5, 6, 7 or 8");
        return false;
    }
    if (p->stop_bits != 1 && p->stop_bits != 2)
    {
        snprintf(err, err_len, "stop bits must be 1 or 2");
        return false;
    }

    {
        dm_v42_mode_t mode;

        if (!parse_v42_mode(p->v42, &mode))
        {
            snprintf(err, err_len, "--v42 must be off, detect or require");
            return false;
        }
        /* V.42 cannot work over V.23's 75 bps back channel. One LAPM frame
         * is 1072 bits, which takes 14 seconds to send at 75 bps, while
         * T401 - the time the far end waits for an acknowledgement - is one
         * second. Every frame would be given up on before it had finished
         * transmitting. V.42 was written for symmetric modems at 1200 bps
         * and above, and V.23 predates it; the combination never existed. */
        if (mode != DM_V42_OFF && mod == DM_MOD_V23)
        {
            snprintf(err, err_len,
                     "--v42 cannot run over V.23: its 75 bps back channel takes 14 seconds to "
                     "send one LAPM frame, and the acknowledgement timer is one second");
            return false;
        }
        if (p->v42bis && mode == DM_V42_OFF)
        {
            /* Compression without error correction is not a thing. V.42bis
             * keeps a dictionary that both ends build as they go; one
             * corrupted byte and the two dictionaries diverge, after which
             * everything that follows is garbage rather than one bad
             * character. It is only safe on top of a corrected link. */
            snprintf(err, err_len, "--v42bis needs --v42 (compression without error correction "
                                   "turns one bad byte into a broken session)");
            return false;
        }
        if (p->v42bis_dict < V42BIS_MIN_DICTIONARY_SIZE || p->v42bis_dict > V42BIS_MAX_CODEWORDS)
        {
            snprintf(err, err_len, "--v42bis-dict must be between %d and %d codewords",
                     V42BIS_MIN_DICTIONARY_SIZE, V42BIS_MAX_CODEWORDS);
            return false;
        }
        if (p->v42bis_max_string < V42BIS_MIN_STRING_SIZE ||
            p->v42bis_max_string > V42BIS_MAX_STRING_SIZE)
        {
            snprintf(err, err_len, "--v42bis-max-string must be between %d and %d",
                     V42BIS_MIN_STRING_SIZE, V42BIS_MAX_STRING_SIZE);
            return false;
        }
    }
    return true;
}

void dm_modem_params_from_config(const dm_config_t *cfg, bool calling, const char *tag,
                                 dm_modem_params_t *out)
{
    memset(out, 0, sizeof(*out));
    out->calling = calling;
    out->modulation = cfg->modulation;
    out->bit_rate = cfg->bit_rate;
    out->guard_tone = cfg->guard_tone;
    out->data_bits = cfg->data_bits;
    out->parity = cfg->parity;
    out->stop_bits = cfg->stop_bits;
    out->v14 = cfg->v14;
    out->tx_power = cfg->tx_power;
    out->answer_tone_ms = cfg->answer_tone_ms;
    out->answer_wait_s = cfg->answer_wait_s;
    out->answer_tail_ms = cfg->answer_tail_ms;
    out->v42 = cfg->v42;
    out->v42_timeout_s = cfg->v42_timeout_s;
    /* A jitter buffer at each end, so the round trip is twice one of them.
     * This is what V.42's detection window has to be stretched by. */
    out->path_delay_ms = cfg->jitter_buffer_ms * 2;
    out->v42bis = cfg->v42bis;
    out->v42bis_dict = cfg->v42bis_dict;
    out->v42bis_max_string = cfg->v42bis_max_string;
    out->calling_tone = cfg->calling_tone;
    out->tag = tag;
}

/* ------------------------------------------------------- data pump glue */

static bool v42_undecided(const dm_modem_t *m)
{
    return m->v42 != NULL && !m->lapm_up && !m->v42_fell_back;
}

/* Everything received ends up here: framed characters from the async path,
 * or the payload of a LAPM frame. Called on the media thread with the lock
 * held. */
static void deliver_rx(dm_modem_t *m, const unsigned char *data, size_t len)
{
    if (len == 0)
        return;
    m->bytes_rx += len;
    m->last_rx_ms = dm_now_ms();
    dm_ring_write_overwrite(&m->rx, data, len);
    m->wake_pending = true;
}

/* Push the "line is busy until" estimate out by however long these bytes
 * will take to transmit. Called on the media thread with the lock held,
 * every time bytes leave our queues for the pump or for LAPM. */
static void charge_line_time(dm_modem_t *m, size_t bytes)
{
    int64_t now = dm_now_ms();
    int rate = m->tx_bit_rate > 0 ? m->tx_bit_rate : 300;

    /* LAPM asks for a frame constantly, and mostly gets nothing. Charging
     * those empty calls would drag the estimate forward to "now" on every
     * one of them, and the line would never be considered drained at all. */
    if (bytes == 0)
        return;

    if (m->line_busy_until_ms < now)
        m->line_busy_until_ms = now;
    m->line_busy_until_ms += (int64_t) bytes * DM_LINE_BITS_PER_BYTE * 1000 / rate;
}

/* The transmit side frames characters itself rather than using spandsp's
 * async_tx. async_tx signals SIG_STATUS_END_OF_DATA as soon as its get_byte
 * callback comes up empty, which tells the data pump to shut the carrier
 * down - correct for a fax burst, fatal for an interactive session where the
 * queue is empty most of the time. A real async modem holds the line at mark
 * between characters instead, which is all the difference below amounts to.
 *
 * Called from the media thread with the modem lock held. */
static int async_framer_get_bit(dm_modem_t *m)
{
    int parity_slot = (m->parity != ASYNC_PARITY_NONE) ? 1 : 0;
    int bit;

    if (m->tx_bitpos == 0)
    {
        int c = dm_ring_getc(&m->tx);

        if (c < 0)
            return 1; /* idle mark */
        m->tx_byte = (unsigned) c & ((1u << m->data_bits) - 1u);
        m->tx_parity = 0;
        m->tx_bitpos = 1;
        m->bytes_tx++;
        m->wire_tx++;
        charge_line_time(m, 1);
        return 0; /* start bit */
    }
    if (m->tx_bitpos <= m->data_bits)
    {
        bit = (int) (m->tx_byte & 1u);
        m->tx_byte >>= 1;
        m->tx_parity ^= bit;
        m->tx_bitpos++;
        return bit;
    }
    if (parity_slot && m->tx_bitpos == m->data_bits + 1)
    {
        m->tx_bitpos++;
        return (m->parity == ASYNC_PARITY_ODD) ? (m->tx_parity ^ 1) : m->tx_parity;
    }
    /* Stop bits, then back round for the next character. */
    if (m->tx_bitpos >= m->data_bits + parity_slot + m->stop_bits)
        m->tx_bitpos = 0;
    else
        m->tx_bitpos++;
    return 1;
}

/* The framing monitor's output is discarded - see arx_monitor. */
static void rx_monitor_byte(void *user, int byte)
{
    (void) user;
    (void) byte;
}

static void rx_put_byte(void *user, int byte)
{
    dm_modem_t *m = user;
    unsigned char c;

    if (byte < 0)
        return; /* async_rx forwards status codes here; handled in rx_status */
    /* Training patterns are scrambled ones, and the character framer will
     * happily turn them into characters. Nothing counts as data until the
     * modems have agreed that they are connected. */
    if (m->phase != DM_PHASE_DATA)
        return;
    c = (unsigned char) byte;
    if (v42_undecided(m))
    {
        /* Might be the far end's banner, might be its ODP pattern. We will
         * know which shortly; hold it until then. */
        dm_ring_write(&m->pending_rx, &c, 1);
        return;
    }
    m->wire_rx++;
    deliver_rx(m, &c, 1);
}

/* ------------------------------------------------------ V.42 and V.42bis
 *
 * Layering, outermost first:
 *
 *   terminal  <->  V.42bis  <->  LAPM (V.42)  <->  data pump  <->  line
 *
 * When V.42 is running, its bit stream replaces the asynchronous character
 * framing entirely - there are no start and stop bits on the line any more,
 * there are HDLC frames. So tx_get_bit and rx_put_bit below are switches
 * rather than the framer itself, and falling back to raw async is a matter
 * of flipping one flag rather than rewiring the pump. */

/* The V.42bis compressor produced some output. It goes into a staging queue
 * rather than straight into a frame, because a flush can produce more than
 * one frame's worth at once. */
static void v42bis_encoded(void *user, const uint8_t *buf, int len)
{
    dm_modem_t *m = user;

    if (len <= 0)
        return;
    if (dm_ring_write(&m->comp_out, buf, (size_t) len) != (size_t) len)
    {
        /* Would mean the headroom calculation above is wrong. Losing bytes
         * here desynchronises the far end's dictionary, so say so loudly
         * rather than let the session quietly turn to noise. */
        DM_ERROR("modem", "compressed output overran its queue (tag=%s); the link is no longer "
                          "trustworthy",
                 m->tag);
    }
}

/* The V.42bis decompressor recovered some plaintext. */
static void v42bis_decoded(void *user, const uint8_t *buf, int len)
{
    dm_modem_t *m = user;

    if (len > 0 && !m->rx_discard)
        deliver_rx(m, buf, (size_t) len);
}

static void v42bis_feed(dm_modem_t *m, const uint8_t *buf, size_t len, bool discard)
{
    if (len == 0)
        return;
    m->rx_discard = discard;
    v42bis_decompress(m->v42bis, buf, (int) len);
    v42bis_decompress_flush(m->v42bis);
    m->rx_discard = false;
}

/* Received data from a far end that never answered our XID.
 *
 * V.42bis starts in transparent mode, where data passes through unchanged,
 * and the only way out of it is the escape character - initially 0x00 -
 * followed by a command: 0 to enter compressed mode, 1 for a literal escape
 * character, 2 to reset. So the first 0x00 settles it. Followed by a command
 * code, the far end is running V.42bis whatever it did not say, and from
 * then on what it sends goes through the decompressor, whose dictionary has
 * been kept in step all along. Followed by anything else, it is not, and the
 * shadow is dropped.
 *
 * Met in the field: a BBS whose modem never answered XID, sent its banner in
 * the clear, then "00 00" and compressed codewords - which, uncompressed,
 * was a screen of garbage two lines in. */
static void v42bis_shadow(dm_modem_t *m, const uint8_t *msg, size_t len)
{
    size_t start = 0;

    for (size_t i = 0; i < len; i++)
    {
        uint8_t c = msg[i];

        if (m->rx_shadow_esc)
        {
            static const uint8_t esc = 0x00;

            m->rx_shadow_esc = false;
            m->rx_shadow = false;
            if (c <= 2)
            {
                /* Everything before the escape was transparent, and has
                 * been delivered and taught to the dictionary already. */
                m->comp_rx = true;
                DM_WARN("modem", "the far end never answered our XID but is sending V.42bis anyway; "
                                 "decompressing it with the parameters we offered (dictionary %d, "
                                 "strings up to %d) (tag=%s)",
                        m->v42bis_dict, m->v42bis_max_string, m->tag);
                v42bis_feed(m, &esc, 1, false);
                v42bis_feed(m, msg + i, len - i, false);
                return;
            }
            DM_DEBUG("modem", "the far end sent a 0x00 that is not a V.42bis escape; it is not "
                              "compressing (tag=%s)", m->tag);
            deliver_rx(m, &esc, 1);
            deliver_rx(m, msg + i, len - i);
            return;
        }
        if (c == 0x00)
        {
            /* Hold it: alone it means nothing yet. The decompressor is fed
             * up to here, and the 0x00 with whatever follows it. */
            deliver_rx(m, msg + start, i - start);
            v42bis_feed(m, msg + start, i - start, true);
            m->rx_shadow_esc = true;
            start = i + 1;
        }
    }
    deliver_rx(m, msg + start, len - start);
    v42bis_feed(m, msg + start, len - start, true);
}

/* LAPM wants a frame to send. */
static int v42_iframe_get(void *user, uint8_t *msg, int max_len)
{
    dm_modem_t *m = user;
    size_t n;

    if (max_len <= 0)
        return 0;

    if (m->v42bis != NULL && m->comp_tx)
    {
        if (dm_ring_space(&m->comp_out) >= DM_COMP_HEADROOM)
        {
            unsigned char plain[DM_COMP_CHUNK];

            n = dm_ring_read(&m->tx, plain, sizeof(plain));
            if (n > 0)
            {
                m->bytes_tx += n;
                v42bis_compress(m->v42bis, plain, (int) n);
                /* Not optional. Without the flush the compressor sits on a
                 * short burst - which is to say, on everything anyone types -
                 * waiting for more input that will not arrive until the far
                 * end has replied to the thing still stuck in the buffer. */
                v42bis_compress_flush(m->v42bis);
            }
        }
        n = dm_ring_read(&m->comp_out, msg, (size_t) max_len);
        m->wire_tx += n;
        charge_line_time(m, n);
        return (int) n;
    }

    n = dm_ring_read(&m->tx, msg, (size_t) max_len);
    m->bytes_tx += n;
    m->wire_tx += n;
    charge_line_time(m, n);
    return (int) n;
}

/* LAPM delivered a frame, already checked and in order. */
static void v42_iframe_put(void *user, const uint8_t *msg, int len)
{
    dm_modem_t *m = user;

    if (len <= 0)
        return; /* status codes are reported through here too */
    m->wire_rx += (size_t) len;
    if (m->v42bis != NULL && m->comp_rx)
    {
        v42bis_feed(m, msg, (size_t) len, false);
    }
    else if (m->v42bis != NULL && m->rx_shadow)
    {
        v42bis_shadow(m, msg, (size_t) len);
    }
    else
    {
        deliver_rx(m, msg, (size_t) len);
    }
}

static const char *protocol_name(const dm_modem_t *m)
{
    if (m->v42 == NULL || m->v42_fell_back)
        return "async";
    if (!m->lapm_up)
        return "negotiating";
    if (m->v42bis == NULL || (!m->comp_tx && !m->comp_rx))
        return "V.42";
    if (m->comp_tx && m->comp_rx)
        return "V.42/V.42bis";
    return m->comp_tx ? "V.42/V.42bis (sending only)" : "V.42/V.42bis (receiving only)";
}

/* spandsp measures every V.42 timer in bit periods - the detection timeout
 * T400, the LAPM retransmission timer T401, the idle poll T403 - and
 * converts from milliseconds with a transmit rate it hardcodes to 28800 bps
 * at init, with no API to change it.
 *
 * At 2400 bps that makes every timer twelve times too long. At 300 bps,
 * ninety-six. Two spandsp instances inflate by the same factor and so talk
 * to each other quite happily, which is exactly why this hides until you
 * meet real equipment: the far end's T401 fires on schedule, it retransmits
 * into a peer that will not answer for another ten or ninety seconds, it
 * runs out of retries, and the link never comes up.
 *
 * The field is a plain int in a structure we allocate ourselves, and there
 * is no other way to reach it. */
static int v42_round_trip_ms(dm_modem_t *m);

/* T401 has to outlast everything between sending a frame and hearing it
 * acknowledged: the frame itself going out, the round trip, the far end
 * thinking about it, and - if it was part way through a frame of its own -
 * that frame, which the acknowledgement has to wait behind. V.42's 1 s
 * assumes a phone line and a fast modem. On a 300 ms RTP path at 9600 it
 * is about right; at 300 bps a single 128-octet frame takes 3.5 s to send,
 * and an acknowledgement could not possibly come back inside a second. */
static void v42_size_t401(dm_modem_t *m, int rate)
{
#if defined(DATAMODEM_VENDORED_V42)
    int rtd_ms = v42_round_trip_ms(m);
    /* 128 octets of information, address, control, two of FCS and a flag,
     * plus an allowance for bit stuffing. */
    int frame_ms = (int) ((int64_t) (128 + 6) * 8 * 1100 / rate);
    int t401 = rtd_ms + 500 + 2 * frame_ms;

    if (t401 < 1000)
        t401 = 1000;
    dm_v42_set_t401(m->v42, t401);
    DM_DEBUG("modem", "V.42 T401 %d ms, for a %d ms round trip at %d bps (tag=%s)", t401, rtd_ms, rate,
             m->tag);
#else
    (void) m;
    (void) rate;
#endif
}

static void v42_set_bit_rate(dm_modem_t *m, int rate)
{
    if (m->v42 == NULL || rate <= 0)
        return;
    m->v42->tx_bit_rate = rate;
    v42_size_t401(m, rate);
    DM_DEBUG("modem", "V.42 timers scaled to %d bps (tag=%s)", rate, m->tag);
}

/* Arms the whole V.42 handshake for a line rate: the LAPM timers, then the
 * detection window on top of them.
 *
 * This has to be redone once the carrier is up, because until then the rate
 * is only an offer. V.22bis offers 2400 and may settle at 1200, and timers
 * armed for 2400 then expire twice as fast as V.42 allows: T401 runs out of
 * retries while the far end is still within its rights to be thinking, and
 * the link dies having trained perfectly. That is why fixing the rate on the
 * command line worked and letting it negotiate did not - the fixed rate was
 * the only way the timers ever matched the line.
 *
 * Only safe before any data has been exchanged: v42_restart() resets the
 * detection state machine along with everything else. */
/* The round trip the V.42 timers have to allow for. V.32 measures the real
 * one during its start-up, and by the time the carrier is up it is known;
 * for everything else, and before then, the two jitter buffers are the best
 * estimate there is. RTP paths of 400-800 ms have been met in practice. */
static int v42_round_trip_ms(dm_modem_t *m)
{
    if (m->v32 != NULL)
    {
        dm_v32_stats_t vs;

        dm_v32_stats(m->v32, &vs);
        if (vs.round_trip_ms > m->path_delay_ms)
            return vs.round_trip_ms;
    }
    if (m->v34 != NULL)
    {
        dm_v34_stats_t vs;

        dm_v34_stats(m->v34, &vs);
        if (vs.round_trip_ms > m->path_delay_ms)
            return vs.round_trip_ms;
    }
    return m->path_delay_ms;
}

static void v42_arm(dm_modem_t *m, int rate)
{
    int rtd_ms;

    if (m->v42 == NULL || rate <= 0)
        return;
    rtd_ms = v42_round_trip_ms(m);
    /* Before the restart, not after: v42_restart latches T400 from the rate,
     * and a timer already loaded from the wrong one stays wrong. */
    v42_set_bit_rate(m, rate);
    /* T401 has to outlast the round trip, or every acknowledgement - and the
     * XID answer that V.42bis rides on - comes back after we have given up
     * on it. v42_set_bit_rate() just sized it; see v42_size_t401(). */
    v42_restart(m->v42);
    /* v42_restart has just armed T400 from the rate above. Replace it with a
     * window sized for this line rate and this audio path - see
     * DM_V42_DETECT_SPEC_MS. */
    if (m->v42->bit_timer > 0)
    {
        int path_ms = DM_V42_DETECT_SPEC_MS + rtd_ms;
        int bits = 2 * DM_V42_DETECT_MIN_BITS + (int) ((int64_t) path_ms * rate / 1000);

        DM_DEBUG("modem", "V.42 detection window %d -> %d bit periods (%.2fs at %d bps, "
                          "allowing %dms for the audio path) (tag=%s)",
                 (int) m->v42->bit_timer, bits, (double) bits / rate, rate, rtd_ms, m->tag);
        m->v42->bit_timer = bits;
    }
}

/* Schedules another attempt at LAPM establishment. Returns false when there
 * is no point: no vendored V.42 to drive it, or --v42-timeout has no room
 * left for an attempt plus a reasonable chance of it being answered. */
static bool v42_retry_establishment(dm_modem_t *m, const char *why)
{
#if defined(DATAMODEM_VENDORED_V42)
    int64_t now = dm_now_ms();
    bool refused = (why != NULL && strstr(why, "DM") != NULL);

    if (m->v42_deadline_ms == 0 || m->v42_retry_at_ms != 0)
        return false;
    if (refused && ++m->v42_refusals > DM_V42_MAX_REFUSALS)
    {
        DM_INFO("modem", "the far end has refused establishment %u times; it means it "
                         "(tag=%s)", m->v42_refusals, m->tag);
        return false;
    }
    /* Room for the wait and for the far end to answer within T401. */
    if (now + DM_V42_RETRY_MS + 1000 > m->v42_deadline_ms)
        return false;

    m->v42_retry_at_ms = now + DM_V42_RETRY_MS;
    DM_INFO("modem", "V.42 establishment attempt %u failed%s%s; trying again in %dms "
                     "(%.1fs of --v42-timeout left) (tag=%s)",
            m->v42_establish_tries + 1,
            (why != NULL && why[0] != '\0') ? " - " : "",
            (why != NULL) ? why : "", DM_V42_RETRY_MS,
            (double) (m->v42_deadline_ms - now) / 1000.0, m->tag);
    return true;
#else
    (void) m;
    (void) why;
    return false;
#endif
}

/* LAPM is up: make V.42bis whatever the XID exchange agreed, before a byte
 * has gone through it.
 *
 * V.42bis is negotiated there and nowhere else. The far end may take a
 * smaller dictionary or shorter strings than we offered, compress in one
 * direction only, or not at all - and if no XID was exchanged at all, V.42bis
 * says compression is off. Running the compressor on what we merely offered,
 * as this used to, hands a far end that agreed to less a stream it cannot
 * decode. Called from v42_status() with the lock held. */
static void apply_v42bis_agreement(dm_modem_t *m)
{
#if defined(DATAMODEM_VENDORED_V42)
    int p0;
    int p1;
    int p2;

    if (m->v42bis == NULL)
        return;
    if (!dm_v42_xid_done(m->v42))
    {
        m->comp_tx = m->comp_rx = false;
        /* Our own direction stays uncompressed: that is what V.42bis says
         * no negotiation means, and a far end that is decompressing anyway
         * passes it through untouched in transparent mode. The other way, be
         * ready for it to compress regardless - some do. */
        m->rx_shadow = true;
        m->rx_shadow_esc = false;
        if (getenv("DATAMODEM_V42BIS_REGARDLESS") != NULL)
        {
            /* Test hook: be that far end - compress, and expect compression,
             * on nothing but our own offer. */
            m->comp_tx = m->comp_rx = true;
            m->rx_shadow = false;
            DM_WARN("modem", "DATAMODEM_V42BIS_REGARDLESS: running V.42bis without an agreement. "
                             "This is a test hook.");
            return;
        }
        DM_WARN("modem", "the far end never answered our XID, so V.42bis was not negotiated and "
                         "this link runs uncompressed (tag=%s)", m->tag);
        /* Two very different situations look the same from here: a far end
         * that does not do XID, and one that answered over a path that
         * damaged the answer. The second will damage the data too. */
        if (dm_v42_fcs_errors > 0)
            DM_WARN("modem", "%u LAPM frame%s arrived damaged meanwhile, so it may well have "
                             "answered - this path is corrupting frames, which will hurt the data "
                             "as well (tag=%s)",
                    dm_v42_fcs_errors, dm_v42_fcs_errors == 1 ? "" : "s", m->tag);
        return;
    }
    p0 = m->v42->config.comp & 3;
    p1 = m->v42->config.comp_dict_size;
    p2 = m->v42->config.comp_max_string;
    /* P0 is written from the side of whoever sent the XID command, which is
     * the calling modem: bit 0 is caller to answerer, bit 1 the reverse. */
    m->comp_tx = (p0 & (m->calling ? 1 : 2)) != 0;
    m->comp_rx = (p0 & (m->calling ? 2 : 1)) != 0;
    if (!m->comp_tx && !m->comp_rx)
    {
        DM_WARN("modem", "the far end declined V.42bis in the XID exchange; error correction "
                         "only (tag=%s)", m->tag);
        return;
    }
    if (p1 != m->v42bis_dict || p2 != m->v42bis_max_string)
    {
        v42bis_state_t *agreed;

        if (p1 < V42BIS_MIN_DICTIONARY_SIZE || p1 > V42BIS_MAX_CODEWORDS ||
            p2 < V42BIS_MIN_STRING_SIZE || p2 > V42BIS_MAX_STRING_SIZE)
        {
            m->comp_tx = m->comp_rx = false;
            DM_WARN("modem", "the far end agreed V.42bis with a dictionary of %d and strings of %d, "
                             "which V.42bis does not allow; running uncompressed (tag=%s)",
                    p1, p2, m->tag);
            return;
        }
        agreed = v42bis_init(NULL, V42BIS_P0_BOTH_DIRECTIONS, p1, p2, v42bis_encoded, m,
                             DM_LAPM_MAX_FRAME, v42bis_decoded, m, V42BIS_MAX_OUTPUT_LENGTH);
        if (agreed == NULL)
        {
            m->comp_tx = m->comp_rx = false;
            DM_ERROR("modem", "could not set V.42bis up as agreed; running uncompressed (tag=%s)", m->tag);
            return;
        }
        v42bis_compression_control(agreed, V42BIS_COMPRESSION_MODE_DYNAMIC);
        v42bis_release(m->v42bis);
        v42bis_free(m->v42bis);
        m->v42bis = agreed;
        m->v42bis_dict = p1;
        m->v42bis_max_string = p2;
    }
    DM_INFO("modem", "V.42bis agreed: dictionary %d, strings up to %d, %s (tag=%s)", p1, p2,
            (m->comp_tx && m->comp_rx) ? "both directions"
                                       : (m->comp_tx ? "our direction only" : "their direction only"),
            m->tag);
#else
    /* The system libspandsp neither sends XID nor applies one, so there is
     * no agreement to apply; both ends must simply be configured alike. */
    m->comp_tx = m->comp_rx = (m->v42bis != NULL);
#endif
}

static void v42_status(void *user, int status)
{
    dm_modem_t *m = user;

    /* Every transition, at debug, because when V.42 goes wrong against real
     * equipment the sequence of these is the only thing that says where. */
    DM_DEBUG("modem", "v42 status %d (%s) (tag=%s)", status,
             lapm_status_to_str(status) ? lapm_status_to_str(status) : "?", m->tag);

    switch (status)
    {
    case SIG_STATUS_LINK_CONNECTED:
        if (!m->lapm_up)
        {
            apply_v42bis_agreement(m);
            m->lapm_up = true;
            /* What the parallel framer collected was V.42's own detection
             * pattern, not data. */
            dm_ring_clear(&m->pending_rx);
            dm_log_event(DM_LOG_INFO, "modem", "error correction established",
                         "tag=%s protocol=%s took_ms=%" PRId64, m->tag, protocol_name(m),
                         m->connect_ms ? dm_now_ms() - m->connect_ms : 0);
        }
        break;

    case SIG_STATUS_LINK_DISCONNECTED:
        /* Two quite different events arrive here.
         *
         * If LAPM was up, the far end has closed a working link. If it was
         * never up, this is spandsp giving up on establishment - T401 has
         * expired N400 times with no answer to our SABME - which is a
         * failure to agree on V.42 at all, not a failure of V.42.
         *
         * The second case is the one that matters against real modems, and
         * it used to fall through to nothing: the handler only looked at
         * the case where lapm_up was already true, so a failed negotiation
         * sat there until the --v42-timeout backstop noticed. Treat it as
         * what it is, and fall back at once. */
        if (m->lapm_up)
        {
            m->lapm_up = false;
            m->rx_busy = false;
            DM_WARN("modem", "the far end closed the error-corrected link (tag=%s)", m->tag);
        }
        else if (!m->v42_peer_declined && !m->v42_fell_back)
        {
            const char *why = "";

#if defined(DATAMODEM_VENDORED_V42)
            why = dm_v42_disconnect_cause;
#endif
            /* Establishment failed, which is not the same as the far end
             * being unable to do V.42 - it answered detection, so it can.
             * spandsp has no more patience to offer here: whether it ran out
             * of SABME retries or was refused outright, it parks in
             * LAPM_IDLE and stays there. Being more patient than that is the
             * whole point of --v42-timeout, so keep trying until it runs
             * out. A far end that refused with DM was within its rights to
             * say "not yet" and is entitled to be ready a moment later. */
            if (v42_retry_establishment(m, why))
                break;
            m->v42_peer_declined = true;
            m->v42_establish_failed = true;
            DM_INFO("modem", "V.42 negotiation failed after %u attempt%s%s%s (tag=%s)",
                    m->v42_establish_tries + 1, m->v42_establish_tries == 0 ? "" : "s",
                    (why != NULL && why[0] != '\0') ? " - " : "",
                    (why != NULL) ? why : "", m->tag);
#if defined(DATAMODEM_VENDORED_V42)
            if (dm_v42_fcs_errors > 0)
                DM_WARN("modem", "%u LAPM frame%s arrived with a bad checksum during the "
                                 "negotiation - the audio path is corrupting frames, which is a "
                                 "different problem from the far end refusing (tag=%s)",
                        dm_v42_fcs_errors, dm_v42_fcs_errors == 1 ? "" : "s", m->tag);
            else
                DM_INFO("modem", "every LAPM frame arrived intact, so the audio path is not the "
                                 "problem - the far end simply would not establish (tag=%s)",
                        m->tag);
#endif
        }
        break;

    case SIG_STATUS_LINK_ERROR:
        m->frame_errors++;
        DM_DEBUG("modem", "LAPM frame error (tag=%s, %u so far)", m->tag, m->frame_errors);
        break;

    default:
        if (status == DM_LAPM_V42_UNSUPPORTED && !m->v42_peer_declined && !m->lapm_up)
        {
            /* Detection finished and found nobody. Waiting out our own
             * timeout on top of that achieves nothing - spandsp has already
             * parked the state machine and will not try again. */
            m->v42_peer_declined = true;
#if defined(DATAMODEM_VENDORED_V42)
            if (dm_v42_adp_no_ec)
            {
                m->v42_adp_declined = true;
                DM_INFO("modem", "the far end's detection pattern asked for no error-correcting "
                                 "protocol, so V.42 is not on offer here - falling back without "
                                 "asking (tag=%s)", m->tag);
            }
            else
#endif
            DM_INFO("modem", "the far end did not answer V.42 detection (tag=%s)", m->tag);
        }
        break;
    }
}

static int tx_get_bit(void *user)
{
    dm_modem_t *m = user;

    if (m->v42 != NULL && !m->v42_fell_back)
        return v42_tx_bit(m->v42);
    return async_framer_get_bit(m);
}

static void note_connected(dm_modem_t *m)
{
    bool first = !m->connected;

    if (m->carrier_lost)
        return; /* already given up on; a new call is the only way back */

    m->connected = true;
    m->carrier_down_ms = 0;
    m->phase = DM_PHASE_DATA;
    if (m->v22 != NULL)
    {
        m->bit_rate = v22bis_get_current_bit_rate(m->v22);
        /* V.22bis is symmetric, so the negotiated rate is the transmit rate
         * too. It can settle below what was offered, and the V.42 timers
         * have to follow it down. */
        m->tx_bit_rate = m->bit_rate;
        v42_set_bit_rate(m, m->tx_bit_rate);
    }
    else if (m->v32 != NULL)
    {
        /* Likewise V.32, which may have settled on 4800, and a retrain can
         * change it in either direction. */
        m->bit_rate = dm_v32_bit_rate(m->v32);
        m->tx_bit_rate = m->bit_rate;
        v42_set_bit_rate(m, m->tx_bit_rate);
    }
    else if (m->v34 != NULL)
    {
        /* V.34's two directions need not run at the same rate. What we
         * report is what we receive at; V.42's timers go by what we send. */
        m->bit_rate = dm_v34_rx_rate(m->v34);
        m->tx_bit_rate = dm_v34_tx_rate(m->v34);
        v42_set_bit_rate(m, m->tx_bit_rate);
    }

    if (first)
    {
        m->connect_ms = dm_now_ms();
        m->last_rx_ms = m->connect_ms;
        /* The clock on the V.42 handshake starts now: before the carrier
         * there was nobody to handshake with. And so do its timers - this is
         * the first moment the line rate is a fact rather than an offer. */
        if (m->v42 != NULL)
        {
            m->v42_deadline_ms = m->connect_ms + (int64_t) m->v42_timeout_s * 1000;
            v42_arm(m, m->tx_bit_rate);
        }
        dm_log_event(DM_LOG_INFO, "modem", "carrier established", "tag=%s modulation=%s rate=%d role=%s",
                     m->tag, m->mod_name, m->bit_rate, m->calling ? "originate" : "answer");
        if (m->bit_rate < m->offered_rate)
            DM_INFO("modem", "the far end would not take %d bps, so this call runs at %d "
                             "(tag=%s)", m->offered_rate, m->bit_rate, m->tag);
    }
    else
    {
        DM_INFO("modem", "carrier recovered at %d bps (tag=%s)", m->bit_rate, m->tag);
    }
    /* Whatever was measured before this carrier belongs to the last one. A
     * training pattern framed as characters is not evidence about the rate. */
    m->rate_check_ms = 0;
    m->rate_bad_windows = 0;
}

static void rx_status(void *user, int status)
{
    dm_modem_t *m = user;

    switch (status)
    {
    case SIG_STATUS_CARRIER_UP:
        DM_DEBUG("modem", "carrier detected (tag=%s)", m->tag);
        /* FSK has no training phase to succeed: the carrier is the connection. */
        if (is_fsk(m->mod))
            note_connected(m);
        break;

    case SIG_STATUS_TRAINING_SUCCEEDED:
        note_connected(m);
        break;

    case SIG_STATUS_TRAINING_FAILED:
        DM_WARN("modem", "training failed, the far end will retry (tag=%s)", m->tag);
        break;

    case SIG_STATUS_MODEM_RETRAIN_OCCURRED:
        m->retrains++;
        if (m->v22 != NULL)
            m->bit_rate = v22bis_get_current_bit_rate(m->v22);
        else if (m->v32 != NULL)
            m->bit_rate = dm_v32_bit_rate(m->v32);
        else if (m->v34 != NULL)
            m->bit_rate = dm_v34_rx_rate(m->v34);
        DM_INFO("modem", "retrained at %d bps (tag=%s, %u so far)", m->bit_rate, m->tag, m->retrains);
        /* A link that settled below what we offered and will not hold is the
         * signature of the two ends having disagreed about the rate. V.22bis
         * settles that question once, during training, and has no way to ask
         * it again - so the only cure is to stop asking it at all. See
         * apply_rate_ceiling(). */
        if (m->v22 != NULL && !m->rate_pinned && m->bit_rate > 0 && m->bit_rate < m->offered_rate)
            m->drop_ceiling_to = m->bit_rate;
        break;

    case SIG_STATUS_CARRIER_DOWN:
        /* Not the end of the call yet. Stop treating what arrives as data,
         * start the clock, and let dm_modem_tx() decide if it stays away.
         * See DM_CARRIER_GRACE_MS. */
        if (m->connected && !m->carrier_lost && m->carrier_down_ms == 0)
        {
            m->carrier_down_ms = dm_now_ms();
            m->phase = DM_PHASE_TRAINING;
            DM_DEBUG("modem", "carrier dropped, allowing %dms for a retrain (tag=%s)",
                     m->carrier_grace_ms, m->tag);
        }
        break;

    case SIG_STATUS_POOR_SIGNAL_QUALITY:
        DM_DEBUG("modem", "poor signal quality (tag=%s)", m->tag);
        break;

    default:
        DM_TRACE("modem", "status %s (tag=%s)", signal_status_to_str(status), m->tag);
        break;
    }

    /* Keep the character framer in step: a carrier that came and went must
     * not leave half a character sitting in it. */
    if (m->arx != NULL)
        async_rx_put_bit(m->arx, status);
    if (m->arx_monitor != NULL)
        async_rx_put_bit(m->arx_monitor, status);
}

static void rx_put_bit(void *user, int bit)
{
    dm_modem_t *m = user;

    if (bit < 0)
    {
        rx_status(m, bit);
        return;
    }
    if (m->v42 != NULL && !m->v42_fell_back)
    {
        v42_rx_bit(m->v42, bit);
        /* In parallel, and only until V.42 is decided: if it turns out the
         * far end never wanted V.42, these are the characters it has been
         * sending us all along and they are owed to the terminal. */
        if (v42_undecided(m))
            async_rx_put_bit(m->arx, bit);
    }
    else
    {
        async_rx_put_bit(m->arx, bit);
    }
    async_rx_put_bit(m->arx_monitor, bit);
}

static void v32_test_renegotiate(dm_modem_t *m)
{
    if (m->reneg_after_ms <= 0 || !m->connected || m->carrier_lost ||
        dm_now_ms() - m->connect_ms < m->reneg_after_ms)
        return;
    m->reneg_after_ms = 0;
    if (!dm_v32_renegotiate(m->v32, m->reneg_rate))
        DM_WARN("modem", "DATAMODEM_V32_RENEGOTIATE: could not ask - not a V.32bis call, or not in data "
                         "(tag=%s)", m->tag);
}

/* DATAMODEM_V34_RENEGOTIATE: the same, for V.34's 11.6. */
static void v34_test_renegotiate(dm_modem_t *m)
{
    if (m->reneg_after_ms <= 0 || !m->connected || m->carrier_lost ||
        dm_now_ms() - m->connect_ms < m->reneg_after_ms)
        return;
    m->reneg_after_ms = 0;
    if (!dm_v34_renegotiate(m->v34, m->reneg_rate))
        DM_WARN("modem", "DATAMODEM_V34_RENEGOTIATE: could not ask - not in data (tag=%s)", m->tag);
}

/* V.32 reports its handshake in its own terms; this puts them in the ones
 * rx_status already understands. Called with the lock held, from inside
 * dm_v32_tx or dm_v32_rx. */
static void v32_event(void *user, dm_v32_event_t ev)
{
    dm_modem_t *m = user;

    switch (ev)
    {
    case DM_V32_TRAINED:
        /* The first time is a connection; after that, a retrain that came
         * good - possibly at a different rate. */
        if (m->connected)
            rx_status(m, SIG_STATUS_MODEM_RETRAIN_OCCURRED);
        rx_status(m, SIG_STATUS_TRAINING_SUCCEEDED);
        break;

    case DM_V32_RETRAINING:
    case DM_V32_CARRIER_DOWN:
        /* Either way no data crosses until the retrain completes, and
         * either way it gets the grace period to do so. */
        rx_status(m, SIG_STATUS_CARRIER_DOWN);
        break;

    case DM_V32_TRAINING_FAILED:
        rx_status(m, SIG_STATUS_TRAINING_FAILED);
        break;

    case DM_V32_RATE_CHANGED:
        /* V.32 bis changed rate without retraining. Nothing was lost and the
         * carrier never went; only the rate, and V.42's timers with it. */
        m->bit_rate = dm_v32_bit_rate(m->v32);
        m->tx_bit_rate = m->bit_rate;
        v42_set_bit_rate(m, m->tx_bit_rate);
        DM_INFO("modem", "rate changed to %d bps (tag=%s)", m->bit_rate, m->tag);
        break;

    case DM_V32_CLEARDOWN:
        /* The two ends found no rate in common, which no retrain will
         * change. */
        if (!m->carrier_lost)
        {
            m->carrier_lost = true;
            m->phase = DM_PHASE_DOWN;
            DM_ERROR("modem", "the V.32 rate exchange called for a cleardown (tag=%s)", m->tag);
        }
        break;
    }
}

/* V.34's events in the terms rx_status understands, as for V.32. */
static void v34_event(void *user, dm_v34_event_t ev)
{
    dm_modem_t *m = user;

    switch (ev)
    {
    case DM_V34_TRAINED:
        if (m->connected)
            rx_status(m, SIG_STATUS_MODEM_RETRAIN_OCCURRED);
        rx_status(m, SIG_STATUS_TRAINING_SUCCEEDED);
        break;
    case DM_V34_RETRAINING:
    case DM_V34_CARRIER_DOWN:
        rx_status(m, SIG_STATUS_CARRIER_DOWN);
        break;
    case DM_V34_TRAINING_FAILED:
        rx_status(m, SIG_STATUS_TRAINING_FAILED);
        break;
    case DM_V34_RATE_CHANGED:
        m->bit_rate = dm_v34_rx_rate(m->v34);
        m->tx_bit_rate = dm_v34_tx_rate(m->v34);
        m->carrier_down_ms = 0;
        m->phase = DM_PHASE_DATA;
        v42_set_bit_rate(m, m->tx_bit_rate);
        DM_INFO("modem", "rate changed to %d bps in, %d out (tag=%s)", m->bit_rate, m->tx_bit_rate, m->tag);
        break;
    case DM_V34_CLEARDOWN:
    case DM_V34_NOT_V34:
        if (!m->carrier_lost)
        {
            m->carrier_lost = true;
            m->phase = DM_PHASE_DOWN;
            DM_ERROR("modem", "%s (tag=%s)",
                     ev == DM_V34_NOT_V34 ? "the far end does not do V.34 (no V.8 exchange)"
                                          : "the V.34 connection was cleared down",
                     m->tag);
        }
        break;
    }
}

/* spandsp's V.42 never reports that detection has failed - point it at a far
 * end that has never heard of V.42 and it will send ODP patterns happily for
 * as long as you let it, with no callback and no give-up. So the deadline
 * lives here. This is also what real modems did: try for a few seconds, then
 * either drop to a direct async connection or give up on the call, depending
 * on how the user set &Q.
 *
 * Called from dm_modem_tx with the lock held. */
/* Which of the two quite different failures this was. They look the same
 * from the session's point of view and are not the same problem: a far end
 * that ignored detection does not do V.42, while one that answered detection
 * and then would not establish does, and is worth more patience. */
static const char *v42_failure_reason(const dm_modem_t *m)
{
    if (m->v42_adp_declined)
        return "its detection pattern asked for no error correction";
    if (m->v42_establish_failed)
        return "it answered detection, but LAPM would not establish";
    if (m->v42_peer_declined)
        return "it did not answer detection";
    return "it said nothing at all";
}

/* Did the two ends actually agree about the line rate?
 *
 * Only asked of a link that settled below what we offered, because that is
 * the only way V.22bis can end up asymmetric - see apply_rate_ceiling(). The
 * test is the async framer's own opinion of what it is being given: a stream
 * demodulated at the wrong rate is noise, and noise puts the stop bit in the
 * wrong place about half the time. Real data, even over a poor line, does
 * not do that. Content-independent, so it does not care what the far end was
 * trying to say. */
static void check_rate_agreement(dm_modem_t *m)
{
    int64_t now;
    int errors;
    uint64_t chars;

    if (m->v22 == NULL || m->arx_monitor == NULL || m->drop_ceiling_to > 0)
        return;
    if (!m->connected || m->phase != DM_PHASE_DATA || m->carrier_lost)
        return;
    /* Once the ceiling has been dropped the rates match by construction, so
     * there is nothing left to decide - but keep measuring, because if the
     * framing is still wrong afterwards then the rate was never the problem
     * and saying so saves somebody the same search. */
    if (!m->rate_pinned && (m->bit_rate <= 0 || m->bit_rate >= m->offered_rate))
        return;
    /* Only once the async framer is what is actually running. While V.42 is
     * still being decided it is framing HDLC in parallel, and complaining
     * about the stop bits in somebody else's flags proves nothing. */
    if (m->v42 != NULL && !m->v42_fell_back)
        return;
    if (m->lapm_up)
        return;

    now = dm_now_ms();
    if (m->rate_check_ms == 0)
    {
        m->rate_check_ms = now;
        m->rate_check_errors = m->arx_monitor->framing_errors;
        m->rate_check_bytes = m->bytes_rx;
        return;
    }
    if (now - m->rate_check_ms < DM_RATE_CHECK_MS)
        return;

    errors = m->arx_monitor->framing_errors - m->rate_check_errors;
    chars = m->bytes_rx - m->rate_check_bytes;
    m->rate_check_ms = now;
    m->rate_check_errors = m->arx_monitor->framing_errors;
    m->rate_check_bytes = m->bytes_rx;

    DM_DEBUG("modem", "rate agreement check: %d framing errors in %" PRIu64 " chars (tag=%s)",
             errors, chars + (uint64_t) errors, m->tag);
    if (chars + (uint64_t) errors < DM_RATE_CHECK_MIN_CHARS)
        return;                 /* too quiet to judge */
    if ((uint64_t) errors * 4 < chars)
    {
        m->rate_bad_windows = 0;
        if (m->rate_pinned && !m->rate_proven)
        {
            m->rate_proven = true;
            DM_INFO("modem", "the framing is clean now, so re-offering %d was the right call "
                             "(tag=%s)", m->bit_rate, m->tag);
        }
        return;                 /* framing is fine, so the rate is fine */
    }

    if (++m->rate_bad_windows < 2)
        return;                 /* one bad window could be a retrain settling */
    if (m->rate_pinned)
    {
        if (!m->rate_complained)
        {
            m->rate_complained = true;
            DM_WARN("modem", "%d of the last %" PRIu64 " characters are still badly framed with "
                             "both ends at %d bps, so the rate was not the problem - the audio "
                             "path is (tag=%s)",
                    errors, chars + (uint64_t) errors, m->bit_rate, m->tag);
        }
        return;
    }
    DM_WARN("modem", "%d of the last %" PRIu64 " characters arrived with the stop bit in the wrong "
                     "place, which is what demodulating the far end at the wrong rate looks like "
                     "(tag=%s)", errors, chars + (uint64_t) errors, m->tag);
    m->drop_ceiling_to = m->bit_rate;
}

static void check_v42_deadline(dm_modem_t *m)
{
    if (m->v42 == NULL || m->lapm_up || m->v42_fell_back || m->v42_deadline_ms == 0)
        return;
    if (m->carrier_lost)
        return;

    /* An establishment retry that has come due. */
    if (m->v42_retry_at_ms != 0 && dm_now_ms() >= m->v42_retry_at_ms)
    {
        m->v42_retry_at_ms = 0;
        m->v42_establish_tries++;
#if defined(DATAMODEM_VENDORED_V42)
        if (dm_v42_reconnect(m->v42) == 0)
        {
            DM_INFO("modem", "V.42 establishment attempt %u: sending SABME again (tag=%s)",
                    m->v42_establish_tries + 1, m->tag);
            return;
        }
        DM_DEBUG("modem", "V.42 is not in a state to retry establishment (tag=%s)", m->tag);
#endif
        m->v42_peer_declined = true;
    }

    /* Either spandsp has told us detection failed, or our own backstop has
     * run out waiting for it to say anything at all. */
    if (!m->v42_peer_declined && dm_now_ms() < m->v42_deadline_ms)
        return;

    if (m->v42_mode == DM_V42_REQUIRE)
    {
        m->carrier_lost = true;
        m->phase = DM_PHASE_DOWN;
        DM_ERROR("modem", "V.42 could not be established (%s) and --v42 require was asked for, "
                          "so the call is no use (tag=%s)",
                 v42_failure_reason(m), m->tag);
        return;
    }

    /* Drop back to a plain start/stop stream. The pump keeps running and the
     * carrier is untouched; only the meaning of the bits changes. */
    m->v42_fell_back = true;
    m->tx_bitpos = 0;
    DM_WARN("modem", "giving up on V.42 (%s); falling back to a direct async connection "
                     "(tag=%s)", v42_failure_reason(m), m->tag);

    /* Whatever the parallel framer collected while we were deciding is
     * either the far end talking plain async all along - worth keeping - or
     * its HDLC, framed by a receiver that had no business framing it. The
     * two are easy to tell apart: a stream of LAPM flags is a quarter 0x7e
     * or more once chopped into characters, and ordinary text is not. Giving
     * HDLC to the terminal is how a failed negotiation ends up looking like
     * line noise, so count first and deliver only if it reads as data. */
    {
        unsigned char held[512];
        size_t n;
        size_t total = 0;
        size_t flags = 0;
        unsigned char kept[DM_PENDING_RX];
        size_t kept_n = 0;

        while ((n = dm_ring_read(&m->pending_rx, held, sizeof(held))) > 0)
        {
            for (size_t i = 0; i < n; i++)
            {
                if (held[i] == 0x7e)
                    flags++;
                if (kept_n < sizeof(kept))
                    kept[kept_n++] = held[i];
            }
            total += n;
        }
        if (total == 0)
        {
            /* nothing was held back */
        }
        else if (flags * 4 >= total)
        {
            DM_INFO("modem", "discarded %zu byte%s of the far end's HDLC that arrived while V.42 "
                             "was still being decided - %zu of them flags, so it was LAPM and "
                             "not data (tag=%s)",
                    total, total == 1 ? "" : "s", flags, m->tag);
        }
        else
        {
            m->wire_rx += kept_n;
            deliver_rx(m, kept, kept_n);
            DM_INFO("modem", "recovered %zu byte%s the far end sent while V.42 was still being "
                             "decided (tag=%s)",
                    kept_n, kept_n == 1 ? "" : "s", m->tag);
        }
    }
}

/* ------------------------------------------------------ create / destroy */

static void start_pump(dm_modem_t *m, const char *why)
{
    if (m->pump_started)
        return;
    m->pump_started = true;
    if (m->v32 != NULL)
        dm_v32_start(m->v32);
    m->phase = DM_PHASE_TRAINING;
    dm_log_event(DM_LOG_INFO, "modem", "training", "tag=%s modulation=%s role=%s offered_rate=%d why=%s",
                 m->tag, m->mod_name, m->calling ? "originate" : "answer", m->offered_rate, why);
}

dm_modem_t *dm_modem_create(const dm_modem_params_t *params)
{
    dm_modem_t *m;
    char err[256];

    if (!dm_modem_params_check(params, err, sizeof(err)))
    {
        DM_ERROR("modem", "%s", err);
        return NULL;
    }

    m = calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;

    pthread_mutex_init(&m->lock, NULL);
    parse_modulation(params->modulation, &m->mod);
    parse_parity(params->parity, &m->parity);
    parse_guard(params->guard_tone, &m->guard);
    m->mod_name = modulation_name(m->mod);
    m->calling = params->calling;
    m->data_bits = params->data_bits;
    m->stop_bits = params->stop_bits;
    m->v14 = params->v14;
    m->tx_power = (float) params->tx_power;
    m->path_delay_ms = params->path_delay_ms;
    m->offered_rate = (m->mod == DM_MOD_V22 || params->bit_rate == 0) ? default_rate(m->mod)
                                                                      : params->bit_rate;
    m->carrier_grace_ms = is_v32(m->mod)             ? DM_CARRIER_GRACE_V32_MS
                          : (m->mod == DM_MOD_V34) ? DM_CARRIER_GRACE_V34_MS
                                                   : DM_CARRIER_GRACE_MS;
    m->tx_bit_rate = m->offered_rate;
    if (is_fsk(m->mod))
    {
        m->offered_rate = fsk_rate_for(m->mod, m->calling, false);
        /* V.23 is not symmetric: the calling end receives at 1200 and
         * transmits at 75, so how long our own data takes to get out is a
         * different number from the one we report as the connection rate. */
        m->tx_bit_rate = fsk_rate_for(m->mod, m->calling, true);
    }
    m->bit_rate = m->offered_rate;
    m->phase = DM_PHASE_IDLE;
    m->last_rx_ms = dm_now_ms();
    snprintf(m->tag, sizeof(m->tag), "%s", params->tag ? params->tag : (params->calling ? "out" : "in"));

    /* The guard tone belongs to the answering modem; a calling modem that
     * sent one would be talking over the channel it needs to listen to. */
    if (m->calling && m->guard != V22BIS_GUARD_TONE_NONE)
    {
        DM_DEBUG("modem", "ignoring the guard tone setting: only the answering modem sends one");
        m->guard = V22BIS_GUARD_TONE_NONE;
    }

    {
        /* Ten bits carries one 8N1 character, near enough for sizing. */
        size_t tx_cap = (size_t) m->offered_rate / 10 * DM_TX_SECONDS;

        if (tx_cap < DM_TX_QUEUE_MIN)
            tx_cap = DM_TX_QUEUE_MIN;
        if (!dm_ring_init(&m->tx, tx_cap) || !dm_ring_init(&m->rx, DM_RX_QUEUE))
        {
            dm_modem_destroy(m);
            return NULL;
        }
    }

    m->arx = async_rx_init(NULL, m->data_bits, m->parity, m->stop_bits, m->v14 ? TRUE : FALSE,
                           rx_put_byte, m);
    m->arx_monitor = async_rx_init(NULL, m->data_bits, m->parity, m->stop_bits, FALSE,
                                   rx_monitor_byte, m);
    if (m->arx == NULL || m->arx_monitor == NULL)
    {
        DM_ERROR("modem", "async_rx_init failed");
        dm_modem_destroy(m);
        return NULL;
    }

    parse_v42_mode(params->v42, &m->v42_mode);
    m->v42_timeout_s = params->v42_timeout_s > 0 ? params->v42_timeout_s : 10;
#if defined(DATAMODEM_VENDORED_V42)
    /* Test hook, not an option: see third_party/spandsp-v42. Reproduces a far
     * end that answers the first few SABMEs with DM. */
    {
        const char *refuse = getenv("DATAMODEM_V42_REFUSE_SABME");

        if (refuse != NULL && *refuse != '\0')
        {
            dm_v42_refuse_sabme = atoi(refuse);
            DM_WARN("modem", "DATAMODEM_V42_REFUSE_SABME=%d: the answering end will refuse that "
                             "many SABMEs with DM. This is a test hook.", dm_v42_refuse_sabme);
        }
        {
            extern int dm_v22bis_deaf_to_s1;

            if (getenv("DATAMODEM_V22BIS_DEAF_TO_S1") != NULL)
            {
                dm_v22bis_deaf_to_s1 = 1;
                DM_WARN("modem", "DATAMODEM_V22BIS_DEAF_TO_S1: the calling end will not hear the "
                                 "answerer's S1, so the two will disagree about the rate. This is "
                                 "a test hook.");
            }
        }
        if (getenv("DATAMODEM_V42_NO_XID") != NULL)
        {
            dm_v42_no_xid = 1;
            DM_WARN("modem", "DATAMODEM_V42_NO_XID: this end will establish V.42 without an XID "
                             "exchange. This is a test hook.");
        }
        if (getenv("DATAMODEM_V42_ANSWER_NO_EC") != NULL)
        {
            dm_v42_answer_no_ec = 1;
            DM_WARN("modem", "DATAMODEM_V42_ANSWER_NO_EC: the answering end will send the "
                             "\"no error-correcting protocol desired\" ADP. This is a test hook.");
        }
    }
#endif
    if (m->v42_mode != DM_V42_OFF)
    {
        /* detect = TRUE: run the V.42 detection phase (ODP from the caller,
         * ADP from the answerer) rather than opening with SABME. It is the
         * standard handshake, and it is what a far end doing auto-reliable
         * is listening for. */
        m->v42 = v42_init(NULL, m->calling ? TRUE : FALSE, TRUE, v42_iframe_get, v42_iframe_put, m);
        if (m->v42 == NULL)
        {
            DM_ERROR("modem", "v42_init failed");
            dm_modem_destroy(m);
            return NULL;
        }
        if (!dm_ring_init(&m->pending_rx, DM_PENDING_RX))
        {
            dm_modem_destroy(m);
            return NULL;
        }
        v42_set_status_callback(m->v42, v42_status, m);
        /* What our XID offers. spandsp's own default is compression in one
         * direction only, with a 512 codeword dictionary and 6-character
         * strings, whatever --v42bis says; and with --v42bis off it should
         * offer none at all. */
        m->v42->config.comp = params->v42bis ? V42BIS_P0_BOTH_DIRECTIONS : 0;
        m->v42->config.comp_dict_size = params->v42bis_dict;
        m->v42->config.comp_max_string = params->v42bis_max_string;
        /* spandsp gives the V.42 context its own logging state and sets it
         * to silent, and there is no accessor to reach it. Without this,
         * every word V.42 has to say about why a negotiation went wrong -
         * its own flow messages and the detection diagnostics in the
         * vendored source - is discarded before anyone can read it. */
        attach_logging(&m->v42->logging, m->tag);
        /* Armed here with the rate we are offering, so the context is in a
         * valid state through training, and armed again for real in
         * note_connected() once the negotiated rate is known. */
        v42_arm(m, m->tx_bit_rate);

        if (params->v42bis)
        {
            if (!dm_ring_init(&m->comp_out, DM_COMP_QUEUE))
            {
                dm_modem_destroy(m);
                return NULL;
            }
            /* max_encode_len is capped at the LAPM frame size so the
             * compressor never hands us a block that cannot go out whole. */
            m->v42bis = v42bis_init(NULL, V42BIS_P0_BOTH_DIRECTIONS, params->v42bis_dict,
                                    params->v42bis_max_string, v42bis_encoded, m, DM_LAPM_MAX_FRAME,
                                    v42bis_decoded, m, V42BIS_MAX_OUTPUT_LENGTH);
            if (m->v42bis == NULL)
            {
                DM_ERROR("modem", "v42bis_init failed (dictionary %d, max string %d)",
                         params->v42bis_dict, params->v42bis_max_string);
                dm_modem_destroy(m);
                return NULL;
            }
            v42bis_compression_control(m->v42bis, V42BIS_COMPRESSION_MODE_DYNAMIC);
            /* An offer. The XID exchange decides what actually runs - see
             * apply_v42bis_agreement(). */
            m->comp_tx = m->comp_rx = true;
            m->v42bis_dict = params->v42bis_dict;
            m->v42bis_max_string = params->v42bis_max_string;
            DM_DEBUG("modem", "offering V.42bis with a dictionary of %d codewords and strings up to %d "
                              "(tag=%s)", params->v42bis_dict, params->v42bis_max_string, m->tag);
        }
    }

    if (is_fsk(m->mod))
    {
        m->fsk_tx = fsk_tx_init(NULL, fsk_spec_for(m->mod, m->calling, true), tx_get_bit, m);
        m->fsk_rx = fsk_rx_init(NULL, fsk_spec_for(m->mod, m->calling, false), FSK_FRAME_MODE_ASYNC,
                                rx_put_bit, m);
        if (m->fsk_tx == NULL || m->fsk_rx == NULL)
        {
            DM_ERROR("modem", "could not start the %s data pump", m->mod_name);
            dm_modem_destroy(m);
            return NULL;
        }
        fsk_tx_power(m->fsk_tx, m->tx_power);
        fsk_rx_set_modem_status_handler(m->fsk_rx, rx_status, m);
    }
    else if (is_v32(m->mod))
    {
        dm_v32_params_t vp;

        memset(&vp, 0, sizeof(vp));
        vp.calling = m->calling;
        vp.v32bis = (m->mod == DM_MOD_V32BIS);
        vp.max_rate = m->offered_rate;
        /* Test hook, not an option: every 9600 bit/s V.32 modem has to be
         * able to fall back to the nonredundant 16-point code (1e), and this
         * is how that path gets exercised against ourselves. */
        vp.trellis = (getenv("DATAMODEM_V32_NO_TRELLIS") == NULL);
        /* Running, silent, through the wait for the answer tone below, so
         * that it can hear AC from an answering modem that sends none. */
        vp.listen_first = m->calling && params->answer_wait_s > 0;
        if (!vp.trellis)
            DM_WARN("modem", "DATAMODEM_V32_NO_TRELLIS: this end will not offer trellis coding%s. "
                             "This is a test hook.",
                    vp.v32bis ? " - ignored by V.32bis, whose rates are all trellis coded" : "");
        /* Test hook, not an option: "5:9600" has the calling end ask for
         * 9600 five seconds into the call, "5:9600:answer" the answering
         * end. It is how V.32 bis's rate renegotiation, which a clean line
         * never needs, gets exercised against ourselves. */
        {
            const char *rn = getenv("DATAMODEM_V32_RENEGOTIATE");
            double secs = 0.0;
            int rate = 0;
            char role[16] = "";

            if (rn != NULL && sscanf(rn, "%lf:%d:%15s", &secs, &rate, role) >= 2 && secs > 0.0 &&
                (strcmp(role, "answer") == 0) == !m->calling)
            {
                m->reneg_after_ms = (int) (secs * 1000.0);
                m->reneg_rate = rate;
                DM_WARN("modem", "DATAMODEM_V32_RENEGOTIATE: this end will ask for %d bps %.1f "
                                 "seconds into the call. This is a test hook.", rate, secs);
            }
        }
        /* Test hook: "answer" or "call" makes that end ignore the far end's
         * requests to change rate, as a far end might that does not do 8/V.32
         * bis properly - so the asking end's give-up-and-retrain gets run. */
        {
            const char *deaf = getenv("DATAMODEM_V32_IGNORE_RENEGOTIATION");

            if (deaf != NULL && strcmp(deaf, m->calling ? "call" : "answer") == 0)
            {
                vp.deaf_to_renegotiation = true;
                DM_WARN("modem", "DATAMODEM_V32_IGNORE_RENEGOTIATION: this end will not hear requests to "
                                 "change rate. This is a test hook.");
            }
        }
        vp.tx_power = m->tx_power;
        vp.tag = m->tag;
        vp.get_bit = tx_get_bit;
        vp.put_bit = rx_put_bit;
        vp.event = v32_event;
        vp.user = m;
        m->v32 = dm_v32_create(&vp);
        if (m->v32 == NULL)
        {
            DM_ERROR("modem", "could not start the %s data pump", m->mod_name);
            dm_modem_destroy(m);
            return NULL;
        }
    }
    else if (m->mod == DM_MOD_V34)
    {
        dm_v34_params_t vp;

        memset(&vp, 0, sizeof(vp));
        vp.calling = m->calling;
        vp.max_rate = m->offered_rate;
        vp.tx_power = m->tx_power;
        vp.lapm = m->v42_mode != DM_V42_OFF;
        /* Test hook, not an option: a mask of the symbol rates to allow,
         * bit 0 2400 ... bit 5 3429, so that each can be exercised. */
        if (getenv("DATAMODEM_V34_SYMBOL_RATES") != NULL)
        {
            vp.symbol_rates = (unsigned) strtoul(getenv("DATAMODEM_V34_SYMBOL_RATES"), NULL, 0);
            DM_WARN("modem", "DATAMODEM_V34_SYMBOL_RATES=0x%x: only those symbol rates. This is a test hook.",
                    vp.symbol_rates);
        }
        /* Test hooks: "high" or "low", and 0 to 10 - what this end asks the
         * far end to transmit with, whatever probing says. */
        vp.pre_emphasis = -1;
        if (getenv("DATAMODEM_V34_CARRIER") != NULL)
        {
            vp.carrier = strcmp(getenv("DATAMODEM_V34_CARRIER"), "high") == 0 ? 2 : 1;
            DM_WARN("modem", "DATAMODEM_V34_CARRIER: asking for the %s carrier. This is a test hook.",
                    vp.carrier == 2 ? "high" : "low");
        }
        /* Test hooks: the trellis code and shaping our receiver asks the far
         * end's transmitter for. */
        if (getenv("DATAMODEM_V34_TRELLIS") != NULL)
        {
            vp.trellis = atoi(getenv("DATAMODEM_V34_TRELLIS"));
            DM_WARN("modem", "DATAMODEM_V34_TRELLIS: asking for the %d-state code. This is a test hook.",
                    vp.trellis);
        }
        if (getenv("DATAMODEM_V34_SHAPING") != NULL)
        {
            vp.shaping = true;
            DM_WARN("modem", "DATAMODEM_V34_SHAPING: asking for expanded shaping. This is a test hook.");
        }
        if (getenv("DATAMODEM_V34_PRE_EMPHASIS") != NULL)
        {
            vp.pre_emphasis = atoi(getenv("DATAMODEM_V34_PRE_EMPHASIS"));
            DM_WARN("modem", "DATAMODEM_V34_PRE_EMPHASIS: asking for filter %d. This is a test hook.",
                    vp.pre_emphasis);
        }
        vp.tag = m->tag;
        vp.get_bit = tx_get_bit;
        vp.put_bit = rx_put_bit;
        vp.event = v34_event;
        {
            const char *rn = getenv("DATAMODEM_V34_RENEGOTIATE");
            double secs = 0.0;
            int rate = 0;
            char role[16] = "";

            if (rn != NULL && sscanf(rn, "%lf:%d:%15s", &secs, &rate, role) >= 2 && secs > 0.0 &&
                (strcmp(role, "answer") == 0) == !m->calling)
            {
                m->reneg_after_ms = (int) (secs * 1000.0);
                m->reneg_rate = rate;
                DM_WARN("modem", "DATAMODEM_V34_RENEGOTIATE: this end will ask for at most %d bps inbound %.1f "
                                 "seconds into the call. This is a test hook.", rate, secs);
            }
        }
        vp.user = m;
        m->v34 = dm_v34_create(&vp);
        if (m->v34 == NULL)
        {
            DM_ERROR("modem", "could not start the %s data pump", m->mod_name);
            dm_modem_destroy(m);
            return NULL;
        }
    }
    else
    {
        m->v22 = v22bis_init(NULL, m->offered_rate, m->guard, m->calling ? TRUE : FALSE, tx_get_bit, m,
                             rx_put_bit, m);
        if (m->v22 == NULL)
        {
            DM_ERROR("modem", "could not start the %s data pump", m->mod_name);
            dm_modem_destroy(m);
            return NULL;
        }
        v22bis_tx_power(m->v22, m->tx_power);
        v22bis_set_modem_status_handler(m->v22, rx_status, m);
        attach_logging(v22bis_get_logging_state(m->v22), m->tag);

#if !defined(DATAMODEM_VENDORED_V22BIS)
        /* Built against the system libspandsp. Many packaged builds of it -
         * including the one Homebrew ships - complete V.22bis training,
         * report a connection, and then deliver a constant 0x55 forever,
         * because the receive equaliser has already diverged. Say so rather
         * than let it look like a bad line. See third_party/spandsp-v22bis.
         * `datamodem selftest --modulation v22bis` settles it either way. */
        DM_WARN("modem", "this build uses the system libspandsp for V.22bis; if it is one of "
                         "the many that train and then carry nothing, expect a constant 0x55. "
                         "Run 'datamodem selftest --modulation v22bis' to check.");
#endif
    }

    /* Pre-carrier signalling. The answering end announces itself with a 2100
     * Hz ANS burst before it starts training, the way V.25 says and the way
     * every piece of equipment on the far end expects; the calling end waits
     * to hear one before it bothers training. */
    m->answer_tone_samples = params->answer_tone_ms * DM_SAMPLE_RATE / 1000;
    if (m->mod == DM_MOD_BELL103 || m->mod == DM_MOD_V34)
    {
        /* Nothing to send: the answering carrier is the tone. */
        m->answer_tone_samples = 0;
    }
    m->wait_samples = params->answer_wait_s * DM_SAMPLE_RATE;
    m->ans_tail_samples = params->answer_tail_ms * DM_SAMPLE_RATE / 1000;
    /* V.32 does not have to sit the answer tone out. 5.4.1 has the caller
     * start AA once it has heard ANS for a second, and its receiver is
     * listening for AC at 600 and 3000 Hz, which 2100 Hz cannot be mistaken
     * for. An answering modem that does V.32 automode (Annex A) listens for
     * AA during its answer tone and is waiting for it. */
    if (is_v32(m->mod))
        m->ans_tail_samples = DM_SAMPLE_RATE;

    /* The answer tone has to finish before the calling modem starts, and
     * starting early does not work: 2100 Hz sits close enough to the
     * answering modem's band that the demodulator locks onto the tone,
     * declares itself trained, and drops again the moment the tone stops.
     *
     * spandsp's detector reports the tone as an event rather than a level,
     * so there is nothing to watch for it stopping - and V.25 does not fix
     * the length anyway, it allows 2.6 to 4 seconds. So the caller waits a
     * fixed tail after hearing it. Being late costs nothing: the answering
     * modem sits in its training pattern until we answer it.
     *
     * Bell 103 is the exception, and skips the whole phase. It has no
     * answer tone: the answering modem simply raises its 2225 Hz mark
     * carrier. There is nothing to detect and nothing to wait out, and an
     * FSK receiver cannot be fooled into a false connection by a tone the
     * way a QAM one can - it either sees a carrier or it does not. */
    if (m->calling)
    {
        /* V.34's answer tone is V.8's ANSam, and listening for it is the
         * data pump's own business. */
        if (m->wait_samples > 0 && m->mod != DM_MOD_BELL103 && m->mod != DM_MOD_V34)
        {
            m->tone_rx = modem_connect_tones_rx_init(NULL, ans_tone_type(m->mod), NULL, NULL);
            if (params->calling_tone)
            {
                m->tone_tx = modem_connect_tones_tx_init(NULL, MODEM_CONNECT_TONES_CALLING_TONE);
                if (m->tone_tx != NULL)
                    DM_DEBUG("modem", "sending the V.25 calling tone while waiting (tag=%s)", m->tag);
            }
            m->phase = DM_PHASE_ANSWER_TONE;
        }
    }
    else if (m->answer_tone_samples > 0)
    {
        /* V.32 has an echo canceller of its own, and the network's would
         * fight it: the phase reversals every 450 ms are V.25's signal for
         * echo cancellers along the path to stand aside. */
        m->tone_tx = modem_connect_tones_tx_init(NULL, is_v32(m->mod) ? MODEM_CONNECT_TONES_ANS_PR
                                                                              : MODEM_CONNECT_TONES_ANS);
        if (m->tone_tx == NULL)
        {
            DM_ERROR("modem", "could not start the answer tone generator");
            dm_modem_destroy(m);
            return NULL;
        }
        m->phase = DM_PHASE_ANSWER_TONE;
    }

    if (m->phase != DM_PHASE_ANSWER_TONE)
        start_pump(m, "no answer-tone phase");

    dm_log_event(DM_LOG_INFO, "modem", "engine started",
                 "tag=%s modulation=%s role=%s rate=%d format=%d%c%d guard=%s answer_tone_ms=%d "
                 "answer_wait_s=%d v42=%s v42bis=%s",
                 m->tag, m->mod_name, m->calling ? "originate" : "answer", m->offered_rate, m->data_bits,
                 m->parity == ASYNC_PARITY_NONE ? 'N' : (m->parity == ASYNC_PARITY_EVEN ? 'E' : 'O'),
                 m->stop_bits, params->guard_tone ? params->guard_tone : "none", params->answer_tone_ms,
                 params->answer_wait_s,
                 m->v42_mode == DM_V42_OFF ? "off" : (m->v42_mode == DM_V42_REQUIRE ? "require"
                                                                                    : "detect"),
                 m->v42bis != NULL ? "on" : "off");
    return m;
}

void dm_modem_destroy(dm_modem_t *m)
{
    if (m == NULL)
        return;

    pthread_mutex_lock(&m->lock);
    dm_v32_free(m->v32);
    dm_v34_free(m->v34);
    if (m->v22 != NULL)
    {
        v22bis_release(m->v22);
        v22bis_free(m->v22);
    }
    if (m->fsk_tx != NULL)
    {
        fsk_tx_release(m->fsk_tx);
        fsk_tx_free(m->fsk_tx);
    }
    if (m->fsk_rx != NULL)
    {
        fsk_rx_release(m->fsk_rx);
        fsk_rx_free(m->fsk_rx);
    }
    if (m->arx != NULL)
    {
        async_rx_release(m->arx);
        async_rx_free(m->arx);
    }
    if (m->arx_monitor != NULL)
    {
        async_rx_release(m->arx_monitor);
        async_rx_free(m->arx_monitor);
    }
    if (m->v42bis != NULL)
    {
        v42bis_release(m->v42bis);
        v42bis_free(m->v42bis);
    }
    if (m->v42 != NULL)
    {
        v42_release(m->v42);
        v42_free(m->v42);
    }
    if (m->tone_tx != NULL)
    {
        modem_connect_tones_tx_release(m->tone_tx);
        modem_connect_tones_tx_free(m->tone_tx);
    }
    if (m->tone_rx != NULL)
    {
        modem_connect_tones_rx_release(m->tone_rx);
        modem_connect_tones_rx_free(m->tone_rx);
    }
    pthread_mutex_unlock(&m->lock);
    pthread_mutex_destroy(&m->lock);

    dm_ring_destroy(&m->tx);
    dm_ring_destroy(&m->rx);
    dm_ring_destroy(&m->comp_out);
    dm_ring_destroy(&m->pending_rx);
    free(m);
}

void dm_modem_set_wake(dm_modem_t *m, void (*wake)(void *user), void *user)
{
    pthread_mutex_lock(&m->lock);
    m->wake = wake;
    m->wake_user = user;
    pthread_mutex_unlock(&m->lock);
}

/* --------------------------------------------------------- audio pumping */

void dm_modem_arm(dm_modem_t *m)
{
    pthread_mutex_lock(&m->lock);
    if (!m->armed)
    {
        m->armed = true;
        m->last_rx_ms = dm_now_ms();
        DM_DEBUG("modem", "call answered, starting the modem (tag=%s)", m->tag);
    }
    pthread_mutex_unlock(&m->lock);
}

bool dm_modem_armed(dm_modem_t *m)
{
    bool a;

    pthread_mutex_lock(&m->lock);
    a = m->armed;
    pthread_mutex_unlock(&m->lock);
    return a;
}

/* Re-offers the line rate we actually ended up at, and retrains.
 *
 * V.22bis negotiates 2400 with the S1 pattern: the calling modem sends it,
 * and the answering modem echoes it back if it agrees. Both decisions are
 * made from what each end heard, within a window that closes for good when
 * training ends. On a phone line that is safe. Over RTP it is not: lose the
 * 100 ms of audio carrying the far end's reply and we conclude 1200 while
 * the far end, having heard our S1 perfectly well, concludes 2400. Each then
 * demodulates the other at the wrong rate. There is no renegotiation in
 * V.22bis to recover with, so the link delivers structured garbage, retrains,
 * and does it again until somebody clears the call.
 *
 * Offering only the rate we settled at removes the question. We no longer
 * send S1, so the answering modem has nothing to agree to and must stay at
 * 1200 - which is exactly the configuration --bit-rate 1200 produces, and
 * which works. It costs one retrain, and only on a call that was failing.
 *
 * Deferred out of the status callback because spandsp raises that from
 * inside v22bis_rx(), on state this would reset underneath it. */
static void apply_rate_ceiling(dm_modem_t *m)
{
    int rate = m->drop_ceiling_to;
    int was = m->offered_rate;

    m->drop_ceiling_to = 0;
    if (m->v22 == NULL || rate <= 0 || m->rate_pinned)
        return;
    m->rate_pinned = true;
    DM_WARN("modem", "the link settled at %d having offered %d and will not hold, which means the "
                     "two ends disagreed about the rate; re-offering %d only, so there is nothing "
                     "left to disagree about (tag=%s)", rate, was, rate, m->tag);
    m->offered_rate = rate;
    m->tx_bit_rate = rate;
    m->bit_rate = rate;
    v42_set_bit_rate(m, rate);
    v22bis_restart(m->v22, rate);
    m->phase = DM_PHASE_TRAINING;
    m->connected = false;
    m->carrier_down_ms = 0;
    m->rate_check_ms = 0;
}

int dm_modem_tx(dm_modem_t *m, int16_t *samples, int max_count)
{
    int n = 0;

    if (m == NULL || max_count <= 0)
        return 0;

    pthread_mutex_lock(&m->lock);
    if (!m->armed)
    {
        /* Ringing, or early media. Stay silent and let no timer run. */
        memset(samples, 0, (size_t) max_count * sizeof(int16_t));
        pthread_mutex_unlock(&m->lock);
        return max_count;
    }
    if (m->drop_ceiling_to > 0)
        apply_rate_ceiling(m);
    if (m->phase == DM_PHASE_ANSWER_TONE)
    {
        if (m->v32 != NULL && m->calling)
        {
            /* Silence, unless a calling tone has been asked for - but it
             * keeps the V.32 receiver's clock running, so that it can hear
             * AC if the answering modem sends that without an answer tone. */
            int16_t tone[160];
            int k = 0;

            dm_v32_tx(m->v32, samples, max_count);
            while (m->tone_tx != NULL && k < max_count)
            {
                int chunk = (max_count - k > 160) ? 160 : max_count - k;
                int got = modem_connect_tones_tx(m->tone_tx, tone, chunk);

                for (int i = 0; i < got; i++)
                    samples[k + i] = saturate16((int32_t) samples[k + i] + tone[i]);
                k += chunk;
            }
            if (dm_v32_started(m->v32))
                start_pump(m, m->answer_tone_seen ? "AC heard" : "AC heard without an answer tone");
        }
        else
        {
            if (m->tone_tx != NULL)
                n = modem_connect_tones_tx(m->tone_tx, samples, max_count);
            if (n < max_count)
                memset(samples + n, 0, (size_t) (max_count - n) * sizeof(int16_t));
        }
        n = max_count;

        /* Transitions happen on frame boundaries. At 20 ms a frame that is
         * far finer than anything these timers measure. */
        if (!m->calling)
        {
            m->answer_tone_samples -= max_count;
            if (m->answer_tone_samples <= 0)
                start_pump(m, "answer tone sent");
        }
        else
        {
            /* Hearing the tone can only ever push the start later, never
             * bring it forward. The detector missing a tone that is really
             * there is a survivable mistake - the fixed wait still covers
             * it. Training early because we wrongly believed the line was
             * quiet is not. */
            m->wait_samples -= max_count;
            if (m->ans_tail_running)
                m->ans_tail_samples -= max_count;

            if (m->wait_samples <= 0 && (!m->ans_tail_running || m->ans_tail_samples <= 0))
            {
                if (!m->answer_tone_seen)
                    DM_DEBUG("modem", "no answer tone heard; training anyway (tag=%s)", m->tag);
                start_pump(m, m->answer_tone_seen ? "answer tone finished" : "answer-tone wait elapsed");
            }
        }
    }
    else
    {
        /* The carrier has been gone longer than a retrain could take, so
         * it is not coming back. Keep transmitting either way: the far end
         * may still be trying, and cutting the signal only makes its
         * recovery harder. */
        check_v42_deadline(m);
        check_rate_agreement(m);

        if (m->carrier_down_ms != 0 && !m->carrier_lost &&
            dm_now_ms() - m->carrier_down_ms > m->carrier_grace_ms)
        {
            m->carrier_lost = true;
            m->phase = DM_PHASE_DOWN;
            dm_log_event(DM_LOG_INFO, "modem", "carrier lost",
                         "tag=%s bytes_tx=%" PRIu64 " bytes_rx=%" PRIu64, m->tag, m->bytes_tx,
                         m->bytes_rx);
        }

        if (m->v32 != NULL)
        {
            v32_test_renegotiate(m);
            n = dm_v32_tx(m->v32, samples, max_count);
        }
        else if (m->v34 != NULL)
        {
            v34_test_renegotiate(m);
            n = dm_v34_tx(m->v34, samples, max_count);
        }
        else if (m->v22 != NULL)
            n = v22bis_tx(m->v22, samples, max_count);
        else if (m->fsk_tx != NULL)
            n = fsk_tx(m->fsk_tx, samples, max_count);
    }

    /* RTP never stops, so silence has to be made explicit. */
    if (n < max_count)
    {
        memset(samples + n, 0, (size_t) (max_count - n) * sizeof(int16_t));
        n = max_count;
    }
    pthread_mutex_unlock(&m->lock);
    return n;
}

/* Caller holds m->lock.
 *
 * The callback runs under the lock deliberately. Dropping the lock first and
 * calling it afterwards would be tidier, but it opens a window in which the
 * session can deregister, close its pipe and return - leaving this thread
 * about to call a function pointer with a pointer to a stack frame that no
 * longer exists. Holding the lock across the call means dm_modem_set_wake()
 * cannot complete while a callback is in flight. It is safe to do because
 * the callback only writes one byte to a non-blocking pipe and never comes
 * back into the modem. */
static void fire_wake_locked(dm_modem_t *m)
{
    if (!m->wake_pending)
        return;
    m->wake_pending = false;
    if (m->wake != NULL)
        m->wake(m->wake_user);
}

/* Receive flow control under V.42: see DM_RX_BUSY_FREE. Once per audio
 * frame, on the media thread with the lock held - not from inside LAPM's own
 * delivery callback, which is where the queue fills. */
static void rx_flow_control(dm_modem_t *m)
{
    size_t free_space;

    if (m->v42 == NULL || !m->lapm_up)
        return;
    free_space = dm_ring_space(&m->rx);
    if (!m->rx_busy && free_space < DM_RX_BUSY_FREE)
    {
        m->rx_busy = true;
        v42_set_local_busy_status(m->v42, TRUE);
        DM_DEBUG("modem", "receive queue nearly full (%zu bytes free); asking the far end to wait "
                          "(tag=%s)",
                 free_space, m->tag);
    }
    else if (m->rx_busy && free_space >= DM_RX_READY_FREE)
    {
        m->rx_busy = false;
        v42_set_local_busy_status(m->v42, FALSE);
        DM_DEBUG("modem", "receive queue drained; letting the far end go on (tag=%s)", m->tag);
    }
}

void dm_modem_rx(dm_modem_t *m, const int16_t *samples, int count)
{
    if (m == NULL || count <= 0)
        return;

    pthread_mutex_lock(&m->lock);
    if (!m->armed)
    {
        /* Ringback is not a modem signal. Listening to it is how you end up
         * detecting a carrier that does not exist. */
        pthread_mutex_unlock(&m->lock);
        return;
    }
    if (m->phase == DM_PHASE_ANSWER_TONE && m->calling)
    {
        if (m->v32 != NULL)
            dm_v32_rx(m->v32, samples, count);
        if (m->tone_rx != NULL && !m->answer_tone_seen)
        {
            modem_connect_tones_rx(m->tone_rx, samples, count);
            if (modem_connect_tones_rx_get(m->tone_rx) != MODEM_CONNECT_TONES_NONE)
            {
                m->answer_tone_seen = true;
                if (m->ans_tail_samples > 0)
                {
                    m->ans_tail_running = true;
                    DM_INFO("modem", "answer tone from the far end; letting it finish (%dms) (tag=%s)",
                            m->ans_tail_samples * 1000 / DM_SAMPLE_RATE, m->tag);
                }
                else
                {
                    start_pump(m, "answer tone heard");
                }
            }
        }
    }
    else if (m->v32 != NULL)
    {
        dm_v32_rx(m->v32, samples, count);
    }
    else if (m->v34 != NULL)
    {
        dm_v34_rx(m->v34, samples, count);
    }
    else if (m->v22 != NULL)
    {
        v22bis_rx(m->v22, samples, count);
    }
    else if (m->fsk_rx != NULL)
    {
        fsk_rx(m->fsk_rx, samples, count);
    }
    rx_flow_control(m);
    fire_wake_locked(m);
    pthread_mutex_unlock(&m->lock);
}

void dm_modem_rx_missing(dm_modem_t *m, int count)
{
    if (m == NULL || count <= 0)
        return;

    pthread_mutex_lock(&m->lock);
    if (!m->armed)
    {
        pthread_mutex_unlock(&m->lock);
        return;
    }
    if (m->v32 != NULL)
        dm_v32_rx_fillin(m->v32, count); /* its clock runs through the answer tone too */
    else if (m->v34 != NULL)
        dm_v34_rx_fillin(m->v34, count);
    else if (m->phase == DM_PHASE_ANSWER_TONE)
        ; /* nothing useful to fake into a tone detector */
    else if (m->v22 != NULL)
        v22bis_rx_fillin(m->v22, count);
    else if (m->fsk_rx != NULL)
        fsk_rx_fillin(m->fsk_rx, count);
    rx_flow_control(m);
    pthread_mutex_unlock(&m->lock);
}

/* ------------------------------------------------------- byte interface */

size_t dm_modem_send(dm_modem_t *m, const void *data, size_t len)
{
    return dm_ring_write(&m->tx, data, len);
}

size_t dm_modem_recv(dm_modem_t *m, void *out, size_t max)
{
    return dm_ring_read(&m->rx, out, max);
}

size_t dm_modem_tx_pending(dm_modem_t *m)
{
    return dm_ring_len(&m->tx) + dm_ring_len(&m->comp_out);
}

bool dm_modem_drained(dm_modem_t *m)
{
    bool quiet;

    if (dm_modem_tx_pending(m) > 0)
        return false;

    pthread_mutex_lock(&m->lock);
    quiet = dm_now_ms() >= m->line_busy_until_ms + DM_LINE_TAIL_MS;
    pthread_mutex_unlock(&m->lock);
    return quiet;
}

size_t dm_modem_tx_space(dm_modem_t *m)
{
    return dm_ring_space(&m->tx);
}

size_t dm_modem_rx_pending(dm_modem_t *m)
{
    return dm_ring_len(&m->rx);
}

void dm_modem_tx_clear(dm_modem_t *m)
{
    dm_ring_clear(&m->tx);
    /* Not comp_out: those bytes are already inside the compressor's view of
     * the stream, and dropping them would leave the far end's dictionary
     * out of step with ours for the rest of the call. */
}

/* ----------------------------------------------------------------- state */

void dm_modem_status(dm_modem_t *m, dm_modem_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (m == NULL)
    {
        out->phase_text = "no modem";
        return;
    }
    pthread_mutex_lock(&m->lock);
    out->phase = m->phase;
    out->modulation = m->mod_name;
    out->bit_rate = m->bit_rate;
    out->offered_rate = m->offered_rate;
    out->connected = m->connected;
    out->carrier_lost = m->carrier_lost;
    out->answer_tone_seen = m->answer_tone_seen;
    out->connect_ms = m->connect_ms;
    out->bytes_tx = m->bytes_tx;
    out->bytes_rx = m->bytes_rx;
    out->wire_tx = m->wire_tx;
    out->wire_rx = m->wire_rx;
    out->retrains = m->retrains;
    if (m->v32 != NULL)
    {
        dm_v32_stats_t vs;

        dm_v32_stats(m->v32, &vs);
        out->rx_power = vs.rx_power;
        out->snr_db = vs.snr_db;
        out->line_trellis = vs.trellis;
        out->line_v32bis = vs.v32bis;
        out->renegotiations = vs.renegotiations;
        out->round_trip_ms = vs.round_trip_ms;
        out->echo_cancelling = vs.echo_canceller;
        out->echo_delay_ms = vs.echo_delay_ms;
        out->echo_return_loss_db = vs.echo_return_loss_db;
        out->echo_cancelled_db = vs.echo_cancelled_db;
        out->train_stage = vs.stage;
    }
    else if (m->v34 != NULL)
    {
        dm_v34_stats_t vs;

        dm_v34_stats(m->v34, &vs);
        out->rx_power = vs.rx_power;
        out->snr_db = vs.snr_db;
        out->line_trellis = true;
        out->renegotiations = vs.renegotiations;
        out->round_trip_ms = vs.round_trip_ms;
        out->echo_cancelling = vs.echo_canceller;
        out->echo_delay_ms = vs.echo_delay_ms;
        out->echo_return_loss_db = vs.echo_return_loss_db;
        out->echo_cancelled_db = vs.echo_cancelled_db;
        out->train_stage = vs.stage;
        out->tx_bit_rate = vs.tx_rate;
        out->symbol_rate = vs.rx_symbol_rate;
        out->tx_symbol_rate = vs.tx_symbol_rate;
    }
    else if (m->v22 != NULL)
        out->rx_power = v22bis_rx_signal_power(m->v22);
    else if (m->fsk_rx != NULL)
        out->rx_power = fsk_rx_signal_power(m->fsk_rx);
    out->lapm_up = m->lapm_up;
    out->v42_fell_back = m->v42_fell_back;
    out->frame_errors = m->frame_errors;
    out->protocol = protocol_name(m);
    out->data_ready = m->connected && !m->carrier_lost &&
                      (m->v42 == NULL || m->lapm_up || m->v42_fell_back);
    pthread_mutex_unlock(&m->lock);

    out->phase_text = dm_modem_phase_name(out->phase);
    out->tx_dropped = dm_ring_dropped(&m->tx);
    out->rx_dropped = dm_ring_dropped(&m->rx);
}

bool dm_modem_data_ready(dm_modem_t *m)
{
    bool ready;

    pthread_mutex_lock(&m->lock);
    ready = m->connected && !m->carrier_lost &&
            (m->v42 == NULL || m->lapm_up || m->v42_fell_back);
    pthread_mutex_unlock(&m->lock);
    return ready;
}

bool dm_modem_connected(dm_modem_t *m)
{
    bool c;

    pthread_mutex_lock(&m->lock);
    c = m->connected && !m->carrier_lost;
    pthread_mutex_unlock(&m->lock);
    return c;
}

dm_modem_phase_t dm_modem_phase(dm_modem_t *m)
{
    dm_modem_phase_t p;

    pthread_mutex_lock(&m->lock);
    p = m->phase;
    pthread_mutex_unlock(&m->lock);
    return p;
}

const char *dm_modem_phase_name(dm_modem_phase_t phase)
{
    switch (phase)
    {
    case DM_PHASE_IDLE:
        return "idle";
    case DM_PHASE_ANSWER_TONE:
        return "answer tone";
    case DM_PHASE_TRAINING:
        return "training";
    case DM_PHASE_DATA:
        return "data";
    case DM_PHASE_DOWN:
        return "carrier down";
    }
    return "?";
}

int64_t dm_modem_since_rx_ms(dm_modem_t *m)
{
    int64_t since;

    pthread_mutex_lock(&m->lock);
    since = dm_now_ms() - m->last_rx_ms;
    pthread_mutex_unlock(&m->lock);
    return since;
}

/* -------------------------------------------------------------- selftest */

#define SELFTEST_CHUNK 160 /* 20 ms at 8 kHz, the same frame pjmedia uses */
#define SELFTEST_MAX_SECONDS 120
#define SELFTEST_BYTES 512

/* The line between the two modems. By default a perfect one - each modem
 * hears exactly what the other sent, in the same 20 ms frame - which is
 * what tells a modulation problem from a line problem.
 *
 * DATAMODEM_SELFTEST_LINE describes a worse one, for the things a perfect
 * line cannot exercise: "delay=150,echo=-10,noise=-40,ulaw". delay is one
 * way, in ms, as two jitter buffers and a network would make it; echo puts
 * each modem's own signal back into its receiver that many dB down, a whole
 * round trip later, the way the far end's hybrid does on a call into the
 * telephone network; noise is white noise in dBm0; ulaw passes everything
 * through G.711; burst=20/3 adds 20 ms of loud noise every 3 seconds, which
 * damages a frame or two without making the line look bad on average - how
 * V.42's recovery gets exercised; cut=20/600 silences the line both ways for
 * 600 ms, 20 seconds in, which a modem can only recover from by retraining. A test hook, not an option - it is how V.32's echo
 * canceller and round-trip measurement get tested without a phone line. */
#define SELFTEST_LINE_MAX (SELFTEST_CHUNK * 128) /* 2.5 s of each direction */

typedef struct
{
    int delay;                /* one way, samples */
    int echo_delay;           /* samples */
    float echo_gain;          /* 0 = no echo */
    bool ulaw;
    awgn_state_t *noise;
    awgn_state_t *burst;
    int burst_len;            /* samples */
    int burst_every;          /* samples */
    long cut_at;              /* line silent from here, both ways, 0 = never */
    int cut_len;
    int16_t hist[2][SELFTEST_LINE_MAX];
    long pos;
} selftest_line_t;

static bool selftest_line_init(selftest_line_t *ln, char *desc, size_t desc_len)
{
    const char *spec = getenv("DATAMODEM_SELFTEST_LINE");
    char buf[256];
    char *tok;
    char *save = NULL;
    double delay_ms = 0.0;
    double echo_db = 0.0;
    double noise_db = 0.0;
    bool echo = false;

    memset(ln, 0, sizeof(*ln));
    snprintf(desc, desc_len, "perfect");
    if (spec == NULL || *spec == '\0')
        return true;

    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok_r(buf, ", ", &save); tok != NULL; tok = strtok_r(NULL, ", ", &save))
    {
        if (strncmp(tok, "delay=", 6) == 0)
            delay_ms = atof(tok + 6);
        else if (strncmp(tok, "echo=", 5) == 0)
        {
            echo_db = atof(tok + 5);
            echo = true;
        }
        else if (strncmp(tok, "noise=", 6) == 0)
            noise_db = atof(tok + 6);
        else if (strcmp(tok, "ulaw") == 0)
            ln->ulaw = true;
        else if (strncmp(tok, "cut=", 4) == 0)
        {
            double at = 0.0, ms = 0.0;

            if (sscanf(tok + 4, "%lf/%lf", &at, &ms) != 2 || at <= 0.0 || ms <= 0.0)
            {
                DM_ERROR("selftest", "DATAMODEM_SELFTEST_LINE: cut= wants seconds/ms, as in cut=20/600");
                return false;
            }
            ln->cut_at = (long) (at * DM_SAMPLE_RATE);
            ln->cut_len = (int) (ms * DM_SAMPLE_RATE / 1000.0);
        }
        else if (strncmp(tok, "burst=", 6) == 0)
        {
            double ms = 0.0;
            double every = 0.0;

            if (sscanf(tok + 6, "%lf/%lf", &ms, &every) != 2 || ms <= 0.0 || every <= 0.0)
            {
                DM_ERROR("selftest", "DATAMODEM_SELFTEST_LINE: burst= wants ms/seconds, as in burst=20/3");
                return false;
            }
            ln->burst_len = (int) (ms * DM_SAMPLE_RATE / 1000.0);
            ln->burst_every = (int) (every * DM_SAMPLE_RATE);
            ln->burst = awgn_init_dbm0(NULL, 7654321, -10.0f);
        }
        else
        {
            DM_ERROR("selftest", "DATAMODEM_SELFTEST_LINE: '%s' is not delay=, echo=, noise=, burst=, cut= or ulaw",
                     tok);
            return false;
        }
    }
    ln->delay = (int) (delay_ms * DM_SAMPLE_RATE / 1000.0);
    /* Back from the far end's line card: twice the one-way path, plus a
     * millisecond of local loop. */
    ln->echo_delay = 2 * ln->delay + DM_SAMPLE_RATE / 1000;
    if (ln->echo_delay >= SELFTEST_LINE_MAX - SELFTEST_CHUNK)
    {
        DM_ERROR("selftest", "DATAMODEM_SELFTEST_LINE: a %.0f ms delay is longer than the selftest's "
                             "line can hold", delay_ms);
        return false;
    }
    if (echo)
        ln->echo_gain = powf(10.0f, (float) echo_db / 20.0f);
    if (noise_db < 0.0)
        ln->noise = awgn_init_dbm0(NULL, 1234567, (float) noise_db);
    snprintf(desc, desc_len, "delay %.0f ms each way, echo %s, noise %s, %s", delay_ms,
             echo ? "on" : "none", ln->noise ? "on" : "none", ln->ulaw ? "G.711 mu-law" : "linear");
    if (echo)
        snprintf(desc + strlen(desc), desc_len - strlen(desc), " (echo %.0f dB, noise %.0f dBm0)",
                 echo_db, noise_db);
    return true;
}

static void selftest_line_free(selftest_line_t *ln)
{
    if (ln->noise != NULL)
        awgn_free(ln->noise);
    if (ln->burst != NULL)
        awgn_free(ln->burst);
}

/* One 20 ms frame each way. Both transmit before either receives, which is
 * the order pjmedia's conference bridge calls a port in - and V.32 measures
 * the round trip on the assumption that it is. */
static void selftest_line_run(selftest_line_t *ln, dm_modem_t *caller, dm_modem_t *answerer)
{
    int16_t out[2][SELFTEST_CHUNK];
    int16_t in[2][SELFTEST_CHUNK];
    dm_modem_t *end[2] = { caller, answerer };

    for (int e = 0; e < 2; e++)
    {
        int n = dm_modem_tx(end[e], out[e], SELFTEST_CHUNK);

        if (n < SELFTEST_CHUNK)
            memset(out[e] + n, 0, (size_t) (SELFTEST_CHUNK - n) * sizeof(int16_t));
    }
    for (int i = 0; i < SELFTEST_CHUNK; i++)
    {
        long t = ln->pos + i;

        ln->hist[0][t % SELFTEST_LINE_MAX] = out[0][i];
        ln->hist[1][t % SELFTEST_LINE_MAX] = out[1][i];
        for (int e = 0; e < 2; e++)
        {
            float x = (t >= ln->delay) ? ln->hist[1 - e][(t - ln->delay) % SELFTEST_LINE_MAX] : 0.0f;

            if (ln->echo_gain > 0.0f && t >= ln->echo_delay)
                x += ln->echo_gain * ln->hist[e][(t - ln->echo_delay) % SELFTEST_LINE_MAX];
            if (ln->noise != NULL)
                x += awgn(ln->noise);
            /* Away from the start, so as to hit data rather than training. */
            if (ln->burst != NULL && t > 15 * DM_SAMPLE_RATE && t % ln->burst_every < ln->burst_len)
                x += awgn(ln->burst);
            if (x > 32767.0f)
                x = 32767.0f;
            else if (x < -32768.0f)
                x = -32768.0f;
            in[e][i] = (int16_t) lrintf(x);
            if (ln->ulaw)
                in[e][i] = ulaw_to_linear(linear_to_ulaw(in[e][i]));
            /* The line going dead for a moment, which only a retrain cures. */
            if (ln->cut_at > 0 && t >= ln->cut_at && t < ln->cut_at + ln->cut_len)
                in[e][i] = 0;
        }
    }
    ln->pos += SELFTEST_CHUNK;
    dm_modem_rx(answerer, in[1], SELFTEST_CHUNK);
    dm_modem_rx(caller, in[0], SELFTEST_CHUNK);
}

static void fill_pattern(unsigned char *p, size_t n, unsigned seed)
{
    /* Printable, so a mismatch is readable in the log, and not a repeating
     * run that a framing slip could hide inside. */
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char) (' ' + ((i * 7 + seed) % 95));
}

/* Says where and how the stream went wrong, because "corrupted" on its own
 * tells you nothing about whether it was a framing slip, a lost character or
 * training noise arriving before the data. */
static bool compare_stream(const char *what, const unsigned char *sent, size_t sent_len,
                           const unsigned char *got, size_t got_len)
{
    size_t n = (got_len < sent_len) ? got_len : sent_len;
    size_t i;
    char a[64];
    char b[64];
    size_t ao = 0;
    size_t bo = 0;

    for (i = 0; i < n && sent[i] == got[i]; i++)
        ;
    if (i == n && got_len == sent_len)
        return true;

    if (i == n)
    {
        DM_ERROR("selftest", "%s: %zu bytes sent, %zu received - the stream is the wrong length", what,
                 sent_len, got_len);
        return false;
    }

    for (size_t k = i; k < i + 12 && k < sent_len && ao < sizeof(a) - 5; k++)
        ao += (size_t) snprintf(a + ao, sizeof(a) - ao, "%02x ", sent[k]);
    for (size_t k = i; k < i + 12 && k < got_len && bo < sizeof(b) - 5; k++)
        bo += (size_t) snprintf(b + bo, sizeof(b) - bo, "%02x ", got[k]);

    {
        size_t bad = 0;

        for (size_t k = i; k < n; k++)
            bad += (sent[k] != got[k]);
        DM_ERROR("selftest", "%s: first difference at byte %zu of %zu (%zu received); %zu of the "
                             "first %zu differ",
                 what, i, sent_len, got_len, bad, n);
    }
    DM_ERROR("selftest", "  sent     %s", a);
    DM_ERROR("selftest", "  received %s", b);
    return false;
}

int dm_modem_selftest(const dm_config_t *cfg)
{
    dm_modem_params_t call_params;
    dm_modem_params_t ans_params;
    dm_modem_t *caller = NULL;
    dm_modem_t *answerer = NULL;
    dm_modem_status_t cs;
    dm_modem_status_t as;
    /* DATAMODEM_SELFTEST_BYTES, a test hook: a longer transfer, to measure
     * error rates too low for 512 bytes to show. */
    size_t nbytes = SELFTEST_BYTES;
    /* DATAMODEM_SELFTEST_ONEWAY, a test hook: the answerer sends nothing.
     * Traffic both ways lets acknowledgements ride on I-frames, which is how
     * a LAPM that threw away every RR it was sent passed this test for as
     * long as it did; one way, like a download, they cannot. */
    size_t nbytes_in;
    /* DATAMODEM_SELFTEST_STALL=n, a test hook: two seconds after the link
     * comes up, stop reading what arrives at the answerer for n seconds - a
     * slow terminal, or the AT prompt - so that the receive queue fills and
     * V.42 has to hold the far end off rather than lose data. */
    long stall_from = -1;
    long stall_until = -1;
    unsigned char *sent_out;
    unsigned char *sent_in;
    unsigned char *got_out;
    unsigned char *got_in;
    size_t out_queued = 0;
    size_t in_queued = 0;
    size_t got_out_len = 0;
    size_t got_in_len = 0;
    static selftest_line_t line;
    char line_desc[160];
    long iterations = 0;
    long max_iterations = (long) SELFTEST_MAX_SECONDS * DM_SAMPLE_RATE / SELFTEST_CHUNK;
    long connected_at = -1;
    long settle_until = 0;    /* ignore what arrives while a rate change drains */
    int settled_offer = 0;
    int rc = DM_EXIT_OK;
    int64_t started;

    if (getenv("DATAMODEM_SELFTEST_BYTES") != NULL)
    {
        long b = atol(getenv("DATAMODEM_SELFTEST_BYTES"));

        if (b < 16 || b > 16 * 1024 * 1024)
        {
            DM_ERROR("selftest", "DATAMODEM_SELFTEST_BYTES must be 16 to 16777216");
            return DM_EXIT_CONFIG;
        }
        nbytes = (size_t) b;
        /* Time for it at 300 bps, plus the handshake. */
        max_iterations += (long) (nbytes * 10 / 300) * DM_SAMPLE_RATE / SELFTEST_CHUNK;
    }
    nbytes_in = (getenv("DATAMODEM_SELFTEST_ONEWAY") != NULL) ? 0 : nbytes;
    sent_out = malloc(nbytes);
    sent_in = malloc(nbytes);
    got_out = malloc(nbytes * 2);
    got_in = malloc(nbytes * 2);
    if (sent_out == NULL || sent_in == NULL || got_out == NULL || got_in == NULL)
    {
        free(sent_out);
        free(sent_in);
        free(got_out);
        free(got_in);
        return DM_EXIT_CONFIG;
    }
    if (!selftest_line_init(&line, line_desc, sizeof(line_desc)))
    {
        free(sent_out);
        free(sent_in);
        free(got_out);
        free(got_in);
        return DM_EXIT_CONFIG;
    }
    dm_modem_params_from_config(cfg, true, "selftest-call", &call_params);
    dm_modem_params_from_config(cfg, false, "selftest-answer", &ans_params);

    caller = dm_modem_create(&call_params);
    answerer = dm_modem_create(&ans_params);
    if (caller == NULL || answerer == NULL)
    {
        dm_modem_destroy(caller);
        dm_modem_destroy(answerer);
        selftest_line_free(&line);
        free(sent_out);
        free(sent_in);
        free(got_out);
        free(got_in);
        return DM_EXIT_CONFIG;
    }

    /* There is no call here to answer, so stand in for one: this harness is
     * the moment the far end picks up. */
    dm_modem_arm(caller);
    dm_modem_arm(answerer);

    fill_pattern(sent_out, nbytes, 0);
    fill_pattern(sent_in, nbytes, 31);

    if (nbytes_in == 0)
        DM_INFO("selftest", "looping two %s modems back to back, %zu bytes from the caller only",
                cfg->modulation, nbytes);
    else
        DM_INFO("selftest", "looping two %s modems back to back, %zu bytes each way", cfg->modulation,
                nbytes);
    if (line.delay > 0 || line.echo_gain > 0.0f || line.noise != NULL || line.burst != NULL || line.ulaw)
        DM_INFO("selftest", "the line between them: %s", line_desc);
    started = dm_now_real_ms();

    while (iterations < max_iterations)
    {
        iterations++;
        /* Keep the clock in step with the audio, so that anything driven by
         * a deadline - --v42-timeout and the establishment retries under it,
         * the carrier grace period, the idle timeout - behaves here the way
         * it will on a call. */
        dm_clock_advance_ms(SELFTEST_CHUNK * 1000 / DM_SAMPLE_RATE);
        selftest_line_run(&line, caller, answerer);

        /* Only start pushing data once both ends have a usable link - which
         * with V.42 means LAPM is up, not merely that a carrier exists. That
         * is what a real session waits for, and it keeps the comparison
         * honest. */
        /* The line rate can change mid-call: a link that settled below what
         * was offered and would not hold gets the ceiling dropped and
         * retrains - see apply_rate_ceiling(). Everything collected so far
         * belongs to a link that was not working, so the measurement starts
         * again once the pipes have drained. Without this the harness scores
         * the recovery against the garbage that preceded it and a working
         * fix looks like a failure. */
        if (connected_at >= 0 && settle_until == 0)
        {
            dm_modem_status(caller, &cs);
            if (settled_offer == 0)
                settled_offer = cs.offered_rate;
            else if (cs.offered_rate != settled_offer && cs.offered_rate > 0)
            {
                DM_INFO("selftest", "the rate on offer changed from %d to %d; starting the "
                                    "transfer again", settled_offer, cs.offered_rate);
                settled_offer = cs.offered_rate;
                settle_until = iterations + 2 * DM_SAMPLE_RATE / SELFTEST_CHUNK;
                out_queued = in_queued = 0;
                got_out_len = got_in_len = 0;
            }
        }
        if (settle_until > 0)
        {
            unsigned char drain[512];

            while (dm_modem_recv(caller, drain, sizeof(drain)) > 0)
                ;
            while (dm_modem_recv(answerer, drain, sizeof(drain)) > 0)
                ;
            if (iterations >= settle_until)
                settle_until = 0;
            continue;
        }

        if (dm_modem_data_ready(caller) && dm_modem_data_ready(answerer))
        {
            if (connected_at < 0)
            {
                dm_modem_status(caller, &cs);
                connected_at = iterations;
                if (getenv("DATAMODEM_SELFTEST_STALL") != NULL)
                {
                    stall_from = iterations + 2L * DM_SAMPLE_RATE / SELFTEST_CHUNK;
                    stall_until = stall_from +
                                  atol(getenv("DATAMODEM_SELFTEST_STALL")) * DM_SAMPLE_RATE / SELFTEST_CHUNK;
                    max_iterations += stall_until - stall_from;
                }
                DM_INFO("selftest", "link up after %.1f simulated seconds (%s)",
                        (double) iterations * SELFTEST_CHUNK / DM_SAMPLE_RATE, cs.protocol);
            }
            out_queued += dm_modem_send(caller, sent_out + out_queued, nbytes - out_queued);
            in_queued += dm_modem_send(answerer, sent_in + in_queued, nbytes_in - in_queued);
        }

        if (iterations == stall_from)
            DM_INFO("selftest", "the answerer stops reading");
        if (iterations == stall_until)
            DM_INFO("selftest", "the answerer reads again");
        if (got_out_len < nbytes * 2 && (iterations < stall_from || iterations >= stall_until))
            got_out_len += dm_modem_recv(answerer, got_out + got_out_len, nbytes * 2 - got_out_len);
        if (got_in_len < nbytes * 2)
            got_in_len += dm_modem_recv(caller, got_in + got_in_len, nbytes * 2 - got_in_len);

        if (got_out_len >= nbytes && got_in_len >= nbytes_in)
            break;
    }

    dm_modem_status(caller, &cs);
    dm_modem_status(answerer, &as);

    if (connected_at < 0)
    {
        DM_ERROR("selftest", "no usable link within %d simulated seconds (caller %s, answerer %s)",
                 SELFTEST_MAX_SECONDS, cs.phase_text, as.phase_text);
        rc = DM_EXIT_NO_CARRIER;
    }
    else if (got_out_len < nbytes || got_in_len < nbytes_in)
    {
        DM_ERROR("selftest", "only %zu/%zu bytes arrived at the answerer and %zu/%zu at the caller",
                 got_out_len, nbytes, got_in_len, nbytes_in);
        rc = DM_EXIT_TIMEOUT;
    }
    else
    {
        bool ok = compare_stream("caller -> answerer", sent_out, nbytes, got_out, got_out_len);
        if (!compare_stream("answerer -> caller", sent_in, nbytes_in, got_in, got_in_len))
            ok = false;
        if (!ok)
            rc = DM_EXIT_NO_CARRIER;
    }

    dm_log_event(rc == DM_EXIT_OK ? DM_LOG_INFO : DM_LOG_ERROR, "selftest", "result",
                 "modulation=%s rate=%d protocol=%s link_seconds=%.1f simulated_seconds=%.1f "
                 "wall_ms=%" PRId64 " out_bytes=%zu in_bytes=%zu wire_bytes=%" PRIu64
                 " compression=%.2f frame_errors=%u exit=%d",
                 cs.modulation, cs.bit_rate, cs.protocol,
                 connected_at < 0 ? -1.0 : (double) connected_at * SELFTEST_CHUNK / DM_SAMPLE_RATE,
                 (double) iterations * SELFTEST_CHUNK / DM_SAMPLE_RATE, dm_now_real_ms() - started, got_out_len,
                 got_in_len, cs.wire_tx, cs.wire_tx ? (double) cs.bytes_tx / cs.wire_tx : 1.0,
                 cs.frame_errors + as.frame_errors, rc);

    dm_modem_destroy(caller);
    dm_modem_destroy(answerer);
    selftest_line_free(&line);
    free(sent_out);
    free(sent_in);
    free(got_out);
    free(got_in);
    return rc;
}
