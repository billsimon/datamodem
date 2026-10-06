#include "datamodem/sip.h"
#include "datamodem/log.h"
#include "datamodem/term.h"
#include "datamodem/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pjsua-lib/pjsua.h>

#define DM_CLOCK_RATE 8000
#define DM_PTIME_MS 20
#define DM_HANGUP_GRACE_MS 3000

/* A pjmedia port that is a modem: the conference bridge pulls 20 ms of modem
 * output from get_frame() and pushes 20 ms of the far end into put_frame(). */
typedef struct
{
    pjmedia_port base;
    dm_modem_t *modem;
    unsigned samples_per_frame;
    pj_timestamp ts;
} dm_modem_port_t;

typedef struct
{
    pjsua_call_id call_id;
    dm_modem_t *modem;
    pj_pool_t *pool;
    dm_modem_port_t *port;
    pjsua_conf_port_id slot;
    bool media_active;
    bool answered;            /* the far end picked up; ringback is over */
    bool disconnected;
    bool inbound;
    int last_status;
    char last_reason[128];
    char remote_uri[256];
    dm_call_party_t party;    /* who is on the other end, see dm_sip_call_party() */
    int64_t started_ms;
    int64_t answered_ms;

    /* RTP arrival watch: the packet counter and when it last moved. */
    unsigned last_rx_pkts;
    int64_t last_rx_change_ms;
} dm_call_t;

/* Threads. pjsua calls the on_* callbacks on its own worker threads, while
 * the main thread places, waits on and clears the call; everything both of
 * them touch - the registration result, the inbound hand-off, g.active and
 * the fields of the call it points to - is under g.lock. Two rules keep that
 * deadlock-free whatever locks pjsua holds when it calls us: no pjsua
 * function is called with g.lock held, and a call context is only freed
 * after g.active stops pointing at it, under the lock, so a callback that
 * finds its context still active holds it alive until it lets go. */
static struct
{
    bool started;
    pjsua_acc_id acc_id;
    dm_config_t cfg;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool reg_done;
    bool reg_ok;
    bool inbound_enabled;
    dm_modem_t *pending_modem; /* handed to the next inbound call */
    dm_call_t *active;         /* one call at a time */
} g = {.acc_id = PJSUA_INVALID_ID, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

/* ---------------------------------------------------------------- helpers */

static pj_str_t pjs(const char *s)
{
    pj_str_t r;
    r.ptr = (char *) s;
    r.slen = s ? (pj_ssize_t) strlen(s) : 0;
    return r;
}

/* The main thread is waiting in dm_sip_wait_ms() for something to change.
 * Callers hold g.lock. */
static void signal_change_locked(void)
{
    pthread_cond_broadcast(&g.cond);
}

/* The context of a pjsua call, locked, if it is still the call we are
 * running - or NULL, unlocked, if it is not: not ours, or already cleared. */
static dm_call_t *call_lock(pjsua_call_id call_id)
{
    dm_call_t *c = pjsua_call_get_user_data(call_id);

    pthread_mutex_lock(&g.lock);
    if (c == NULL || c != g.active)
    {
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    return c;
}

static void call_unlock(void)
{
    pthread_mutex_unlock(&g.lock);
}

/* The main thread's view of the call, all at once. */
typedef struct
{
    bool exists;
    bool media_active;
    bool answered;
    bool disconnected;
    pjsua_call_id call_id;
} call_view_t;

static call_view_t call_view(void)
{
    call_view_t v = { false, false, false, false, PJSUA_INVALID_ID };

    pthread_mutex_lock(&g.lock);
    if (g.active != NULL)
    {
        v.exists = true;
        v.media_active = g.active->media_active;
        v.answered = g.active->answered;
        v.disconnected = g.active->disconnected;
        v.call_id = g.active->call_id;
    }
    pthread_mutex_unlock(&g.lock);
    return v;
}

void dm_sip_wait_ms(int ms)
{
    struct timespec ts;

    /* The main thread's waits are where the status line keeps up. */
    dm_term_tick();

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long) (ms % 1000) * 1000000L;
    ts.tv_sec += ms / 1000 + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&g.lock);
    pthread_cond_timedwait(&g.cond, &g.lock, &ts);
    pthread_mutex_unlock(&g.lock);
}

static void log_pj_error(const char *what, pj_status_t status)
{
    char buf[PJ_ERR_MSG_SIZE];
    pj_strerror(status, buf, sizeof(buf));
    DM_ERROR("sip", "%s failed: %s (%d)", what, buf, (int) status);
}

/* ------------------------------------------------------------ media port  */

static pj_status_t modem_port_get_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    dm_modem_port_t *p = (dm_modem_port_t *) port;
    int16_t *buf = (int16_t *) frame->buf;
    unsigned spf = p->samples_per_frame;
    int n;

    n = dm_modem_tx(p->modem, buf, (int) spf);
    if (n < (int) spf)
        memset(buf + n, 0, (spf - (unsigned) n) * sizeof(int16_t));

    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame->size = spf * sizeof(int16_t);
    frame->timestamp.u64 = p->ts.u64;
    p->ts.u64 += spf;
    return PJ_SUCCESS;
}

static pj_status_t modem_port_put_frame(pjmedia_port *port, pjmedia_frame *frame)
{
    dm_modem_port_t *p = (dm_modem_port_t *) port;

    if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO && frame->buf != NULL && frame->size > 0)
        dm_modem_rx(p->modem, (const int16_t *) frame->buf, (int) (frame->size / sizeof(int16_t)));
    else
        dm_modem_rx_missing(p->modem, (int) p->samples_per_frame); /* keep the modem's clock running */
    return PJ_SUCCESS;
}

static pj_status_t modem_port_on_destroy(pjmedia_port *port)
{
    (void) port;
    return PJ_SUCCESS;
}

/* One audio stream and nothing else. pjsua defaults txt_cnt to 1, which puts
 * a T.140 "m=text" line in the SDP that nothing on a dial-up line wants. */
static void modem_call_setting(pjsua_call_setting *cs)
{
    pjsua_call_setting_default(cs);
    cs->aud_cnt = 1;
    cs->vid_cnt = 0;
    cs->txt_cnt = 0;
}

static dm_modem_port_t *create_modem_port(pj_pool_t *pool, dm_modem_t *modem)
{
    dm_modem_port_t *p = PJ_POOL_ZALLOC_T(pool, dm_modem_port_t);
    pj_str_t name = pjs("datamodem");
    unsigned spf = DM_CLOCK_RATE * DM_PTIME_MS / 1000;

    if (p == NULL)
        return NULL;
    pjmedia_port_info_init(&p->base.info, &name, PJMEDIA_SIG_CLASS_PORT_AUD('D', 'M'), DM_CLOCK_RATE, 1, 16,
                           spf);
    p->base.get_frame = &modem_port_get_frame;
    p->base.put_frame = &modem_port_put_frame;
    p->base.on_destroy = &modem_port_on_destroy;
    p->samples_per_frame = spf;
    p->modem = modem;
    return p;
}

/* --------------------------------------------------------------- call ctx */

static dm_call_t *call_create(dm_modem_t *modem, bool inbound)
{
    dm_call_t *c = calloc(1, sizeof(*c));
    pj_status_t status;

    if (c == NULL)
        return NULL;

    c->call_id = PJSUA_INVALID_ID;
    c->slot = PJSUA_INVALID_ID;
    c->modem = modem;
    c->inbound = inbound;
    c->started_ms = dm_now_ms();
    c->last_rx_change_ms = c->started_ms;

    c->pool = pjsua_pool_create("dmcall", 1024, 1024);
    if (c->pool == NULL)
    {
        free(c);
        return NULL;
    }

    c->port = create_modem_port(c->pool, modem);
    if (c->port == NULL)
    {
        pj_pool_release(c->pool);
        free(c);
        return NULL;
    }

    status = pjsua_conf_add_port(c->pool, &c->port->base, &c->slot);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_conf_add_port", status);
        pj_pool_release(c->pool);
        free(c);
        return NULL;
    }
    DM_DEBUG("sip", "modem media port attached to the bridge at slot %d", (int) c->slot);
    return c;
}

/* The caller has already taken c out of g.active, under the lock. */
static void call_destroy(dm_call_t *c)
{
    if (c == NULL)
        return;
    /* pjsua may still hold the call - a BYE that was never answered - and
     * would hand this pointer to the next callback. */
    if (c->call_id != PJSUA_INVALID_ID && pjsua_call_is_active(c->call_id) &&
        pjsua_call_get_user_data(c->call_id) == c)
        pjsua_call_set_user_data(c->call_id, NULL);
    if (c->slot != PJSUA_INVALID_ID)
        pjsua_conf_remove_port(c->slot);
    if (c->pool != NULL)
        pj_pool_release(c->pool);
    free(c);
}

/* -------------------------------------------------------------- callbacks */

/* Let the modem start only once the call is genuinely up.
 *
 * Media becoming active is not the same event as the far end answering: a
 * 183 with SDP gives us a live audio path carrying ringback, and the
 * conference bridge will happily clock our port through all of it. A modem
 * armed that early spends the ringing counting down its answer-tone wait,
 * then trains against ringing tone and announces a carrier that does not
 * exist. Both conditions, or nothing. */
static void arm_if_ready(dm_call_t *c)
{
    if (c->media_active && c->answered && c->modem != NULL)
        dm_modem_arm(c->modem);
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    dm_call_t *c;
    pjsua_call_info ci;

    (void) e;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    DM_INFO("sip", "call %d state %.*s (%d %.*s)", (int) call_id, (int) ci.state_text.slen,
            ci.state_text.ptr, ci.last_status, (int) ci.last_status_text.slen, ci.last_status_text.ptr);

    c = call_lock(call_id);
    if (c == NULL)
        return;

    c->last_status = ci.last_status;
    snprintf(c->last_reason, sizeof(c->last_reason), "%.*s", (int) ci.last_status_text.slen,
             ci.last_status_text.ptr);

    if (ci.state == PJSIP_INV_STATE_EARLY && !c->inbound && (ci.last_status == 180 || ci.last_status == 183))
        dm_term_state("RINGING", "%d %s", ci.last_status, c->last_reason);
    else if (ci.state == PJSIP_INV_STATE_CONFIRMED && !c->inbound)
        dm_term_state("ANSWERED", "starting the modem");
    else if (ci.state == PJSIP_INV_STATE_DISCONNECTED)
        dm_term_state("HUNG UP", "%d %s", ci.last_status, c->last_reason);

    if (ci.state == PJSIP_INV_STATE_CONFIRMED && c->answered_ms == 0)
    {
        c->answered_ms = dm_now_ms();
        c->answered = true;
        arm_if_ready(c);
    }
    if (ci.state == PJSIP_INV_STATE_DISCONNECTED)
        c->disconnected = true;
    signal_change_locked();
    call_unlock();
}

static void on_call_media_state(pjsua_call_id call_id)
{
    dm_call_t *c;
    pjsua_call_info ci;
    pjsua_conf_port_id slot;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
        return;

    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE)
    {
        pjmedia_transport_info tp_info;

        c = call_lock(call_id);
        if (c == NULL)
            return;
        slot = c->slot;
        call_unlock();

        /* Not under the lock: see the comment on g. */
        pjsua_conf_connect(ci.conf_slot, slot);
        pjsua_conf_connect(slot, ci.conf_slot);

        c = call_lock(call_id);
        if (c == NULL)
            return;
        c->media_active = true;
        /* The RTP watchdog only starts counting once there is media to wait for. */
        c->last_rx_change_ms = dm_now_ms();
        arm_if_ready(c);
        signal_change_locked();
        call_unlock();

        pjmedia_transport_info_init(&tp_info);
        if (pjsua_call_get_med_transport_info(call_id, 0, &tp_info) == PJ_SUCCESS)
        {
            char addr[PJ_INET6_ADDRSTRLEN + 10];
            pj_sockaddr_print(&tp_info.sock_info.rtp_addr_name, addr, sizeof(addr), 3);
            DM_INFO("sip", "media active on call %d, modem attached (local RTP %s)", (int) call_id, addr);
        }
        else
        {
            DM_INFO("sip", "media active on call %d, modem attached", (int) call_id);
        }
    }
    else
    {
        DM_DEBUG("sip", "call %d media status %d", (int) call_id, (int) ci.media_status);
    }
}

/* Caller ID. The From header is whatever the caller says it is; a trunk
 * that knows better says so in P-Asserted-Identity (RFC 3325), which is
 * preferred when present. The number called is the user part of To. */
static void read_party(const pjsua_call_info *ci, pjsip_rx_data *rdata, dm_call_party_t *party)
{
    char buf[256];
    char name[sizeof(party->remote_name)];
    char number[sizeof(party->remote_number)];

    memset(party, 0, sizeof(*party));
    snprintf(buf, sizeof(buf), "%.*s", (int) ci->remote_info.slen, ci->remote_info.ptr);
    snprintf(party->remote_uri, sizeof(party->remote_uri), "%s", buf);
    dm_parse_sip_party(buf, party->remote_name, sizeof(party->remote_name), party->remote_number,
                       sizeof(party->remote_number));

    if (rdata != NULL && rdata->msg_info.msg != NULL)
    {
        const pj_str_t pai_name = {"P-Asserted-Identity", 19};
        const pjsip_generic_string_hdr *pai = (const pjsip_generic_string_hdr *) pjsip_msg_find_hdr_by_name(
            rdata->msg_info.msg, &pai_name, NULL);

        if (pai != NULL)
        {
            snprintf(buf, sizeof(buf), "%.*s", (int) pai->hvalue.slen, pai->hvalue.ptr);
            dm_parse_sip_party(buf, name, sizeof(name), number, sizeof(number));
            if (number[0] != '\0')
            {
                snprintf(party->remote_number, sizeof(party->remote_number), "%s", number);
                if (name[0] != '\0')
                    snprintf(party->remote_name, sizeof(party->remote_name), "%s", name);
            }
        }
    }

    snprintf(buf, sizeof(buf), "%.*s", (int) ci->local_info.slen, ci->local_info.ptr);
    dm_parse_sip_party(buf, name, sizeof(name), party->local_number, sizeof(party->local_number));
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata)
{
    pjsua_call_info ci;
    pjsua_call_setting answer_cfg;
    dm_call_t *c;
    dm_modem_t *modem;
    char from[200];

    (void) acc_id;

    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS)
    {
        pjsua_call_hangup(call_id, 500, NULL, NULL);
        return;
    }
    snprintf(from, sizeof(from), "%.*s", (int) ci.remote_info.slen, ci.remote_info.ptr);

    /* Claim the modem, under the lock, so that dm_sip_answer() giving up
     * and this call arriving cannot both have it. */
    pthread_mutex_lock(&g.lock);
    if (!g.inbound_enabled || g.pending_modem == NULL)
    {
        /* An answering modem between calls - finishing one, about to wait
         * for the next - is busy, as a modem still off hook would be. */
        bool answering = (g.cfg.command == DM_CMD_ANSWER);

        pthread_mutex_unlock(&g.lock);
        DM_INFO("sip", "rejecting inbound call from %s: %s", from,
                answering ? "busy between calls" : "not accepting calls");
        pjsua_call_hangup(call_id, answering ? PJSIP_SC_BUSY_HERE : PJSIP_SC_NOT_ACCEPTABLE_HERE, NULL,
                          NULL);
        return;
    }
    if (g.active != NULL)
    {
        pthread_mutex_unlock(&g.lock);
        DM_WARN("sip", "rejecting inbound call from %s: another call is in progress", from);
        pjsua_call_hangup(call_id, PJSIP_SC_BUSY_HERE, NULL, NULL);
        return;
    }
    modem = g.pending_modem;
    g.pending_modem = NULL;
    g.inbound_enabled = false; /* one call per run */
    pthread_mutex_unlock(&g.lock);

    DM_INFO("sip", "inbound call %d from %s, answering", (int) call_id, from);
    dm_term_state("ANSWERING", "%s", from);

    c = call_create(modem, true);
    if (c == NULL)
    {
        DM_ERROR("sip", "could not attach the modem, rejecting call %d", (int) call_id);
        pjsua_call_hangup(call_id, PJSIP_SC_INTERNAL_SERVER_ERROR, NULL, NULL);
        pthread_mutex_lock(&g.lock);
        g.pending_modem = modem;
        g.inbound_enabled = true;
        pthread_mutex_unlock(&g.lock);
        return;
    }
    c->call_id = call_id;
    snprintf(c->remote_uri, sizeof(c->remote_uri), "%s", from);
    read_party(&ci, rdata, &c->party);
    DM_INFO("sip", "caller id: number=\"%s\" name=\"%s\" called=\"%s\"", c->party.remote_number,
            c->party.remote_name, c->party.local_number);
    /* Before it is active, so no callback can find it half set up. */
    pjsua_call_set_user_data(call_id, c);
    pthread_mutex_lock(&g.lock);
    g.active = c;
    pthread_mutex_unlock(&g.lock);

    modem_call_setting(&answer_cfg);
    pjsua_call_answer2(call_id, &answer_cfg, 180, NULL, NULL);
    pjsua_call_answer2(call_id, &answer_cfg, 200, NULL, NULL);
    /* We just sent the 200; there is no ringback to mistake for a carrier on
     * an inbound call. on_call_state will confirm, but do not make the modem
     * wait for it. */
    c = call_lock(call_id);
    if (c != NULL)
    {
        c->answered = true;
        arm_if_ready(c);
        signal_change_locked();
        call_unlock();
    }
}

/* pjmedia adds a telephone-event (RFC 2833) payload to every audio stream at
 * compile time, so there is no setting to turn it off - the SDP has to be
 * edited on its way out. A modem carries its own signalling in the audio
 * band; offering a DTMF payload we will never use only gives a trunk
 * something else to negotiate over. */
static void strip_telephone_event(pjmedia_sdp_media *m)
{
    unsigned i = 0;

    while (i < m->desc.fmt_count)
    {
        pjmedia_sdp_attr *rtpmap_attr = pjmedia_sdp_media_find_attr2(m, "rtpmap", &m->desc.fmt[i]);
        pjmedia_sdp_attr *fmtp_attr;
        pjmedia_sdp_rtpmap rtpmap;

        if (rtpmap_attr == NULL || pjmedia_sdp_attr_get_rtpmap(rtpmap_attr, &rtpmap) != PJ_SUCCESS ||
            pj_stricmp2(&rtpmap.enc_name, "telephone-event") != 0)
        {
            i++;
            continue;
        }

        DM_DEBUG("sip", "dropping telephone-event payload %.*s from the SDP", (int) m->desc.fmt[i].slen,
                 m->desc.fmt[i].ptr);

        fmtp_attr = pjmedia_sdp_media_find_attr2(m, "fmtp", &m->desc.fmt[i]);
        pjmedia_sdp_media_remove_attr(m, rtpmap_attr);
        if (fmtp_attr != NULL)
            pjmedia_sdp_media_remove_attr(m, fmtp_attr);

        for (unsigned k = i + 1; k < m->desc.fmt_count; k++)
            m->desc.fmt[k - 1] = m->desc.fmt[k];
        m->desc.fmt_count--;
    }
}

static void on_call_sdp_created(pjsua_call_id call_id, pjmedia_sdp_session *sdp, pj_pool_t *pool,
                                const pjmedia_sdp_session *rem_sdp)
{
    (void) call_id;
    (void) pool;
    (void) rem_sdp; /* non-NULL when this SDP is an answer; strip it either way */

    for (unsigned i = 0; i < sdp->media_count; i++)
    {
        if (pj_stricmp2(&sdp->media[i]->desc.media, "audio") == 0)
            strip_telephone_event(sdp->media[i]);
    }
}

static void on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info)
{
    struct pjsip_regc_cbparam *rp = info->cbparam;

    bool ok = (rp->code / 100 == 2) && rp->expiration > 0;

    (void) acc_id;
    pthread_mutex_lock(&g.lock);
    g.reg_done = true;
    g.reg_ok = ok;
    signal_change_locked();
    pthread_mutex_unlock(&g.lock);

    if (ok)
        DM_INFO("sip", "registered as %s (expires in %ds)", g.cfg.username, (int) rp->expiration);
    else if (rp->code / 100 == 2)
        DM_INFO("sip", "unregistered (%d)", rp->code);
    else
        DM_ERROR("sip", "registration failed: %d %.*s", rp->code, (int) rp->reason.slen, rp->reason.ptr);
}

/* ------------------------------------------------------------------ setup */

static bool reg_finished(void)
{
    bool done;

    pthread_mutex_lock(&g.lock);
    done = g.reg_done;
    pthread_mutex_unlock(&g.lock);
    return done;
}

static int pjsip_level_for(const dm_config_t *cfg)
{
    if (cfg->pjsip_log_level >= 0)
        return cfg->pjsip_log_level;
    /* pjsip's level 3 includes periodic RTCP dumps for every call, which
     * drowns out our own output; keep it at warnings until --log-level debug. */
    switch (cfg->log_level)
    {
    case DM_LOG_ERROR:
        return 1;
    case DM_LOG_WARN:
    case DM_LOG_INFO:
        return 2;
    case DM_LOG_DEBUG:
        return 4;
    default:
        return 6;
    }
}

static void tune_codecs(const dm_config_t *cfg)
{
    pjsua_codec_info codecs[64];
    unsigned count = PJ_ARRAY_SIZE(codecs);
    bool want_pcma = (strcasecmp(cfg->codec, "pcma") == 0);

    if (pjsua_enum_codecs(codecs, &count) != PJ_SUCCESS)
        return;

    for (unsigned i = 0; i < count; i++)
    {
        const pj_str_t *id = &codecs[i].codec_id;
        pj_uint8_t prio = 0;
        bool is_g711 = false;

        if (pj_strnicmp2(id, "pcmu/", 5) == 0)
        {
            prio = want_pcma ? 254 : 255;
            is_g711 = true;
        }
        else if (pj_strnicmp2(id, "pcma/", 5) == 0)
        {
            prio = want_pcma ? 255 : 254;
            is_g711 = true;
        }

        pjsua_codec_set_priority(id, prio);
        DM_DEBUG("sip", "codec %.*s priority %d", (int) id->slen, id->ptr, (int) prio);

        if (is_g711)
        {
            /* Packet loss concealment invents audio and perceptual
             * enhancement reshapes it. Either one turns a clean
             * constellation into noise. */
            pjmedia_codec_param param;
            if (pjsua_codec_get_param(id, &param) == PJ_SUCCESS)
            {
                param.setting.vad = 0;
                param.setting.plc = 0;
                param.setting.penh = 0;
                if (pjsua_codec_set_param(id, &param) == PJ_SUCCESS)
                    DM_DEBUG("sip", "codec %.*s: vad, plc and penh disabled", (int) id->slen, id->ptr);
            }
        }
    }
    DM_INFO("sip", "offering G.711 only (%s first); a compressed codec cannot carry a modem signal",
            want_pcma ? "PCMA" : "PCMU");
}

int dm_sip_start(const dm_config_t *cfg)
{
    pjsua_config ua_cfg;
    pjsua_logging_config log_cfg;
    pjsua_media_config med_cfg;
    pjsua_transport_config tp_cfg;
    pjsua_acc_config acc_cfg;
    pjsip_transport_type_e tp_type = PJSIP_TRANSPORT_UDP;
    pjsua_transport_id tp_id;
    pj_status_t status;
    char id_uri[DM_STR_MAX * 2];
    char reg_uri[DM_STR_MAX + 8];
    char proxy_uri[DM_STR_MAX];

    g.cfg = *cfg;

    /* pjsua_create() logs before pjsua_init() installs log_cfg.cb, so claim
     * pjlib's writer first - otherwise those first lines land on the user's
     * terminal raw, unformatted and unfiltered. */
    pj_log_set_log_func(&dm_log_pjsip_writer);
    pj_log_set_level(pjsip_level_for(cfg));

    status = pjsua_create();
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_create", status);
        return DM_EXIT_INTERNAL;
    }
    g.started = true;

    pjsua_config_default(&ua_cfg);
    ua_cfg.max_calls = 2;
    ua_cfg.cb.on_call_state = &on_call_state;
    ua_cfg.cb.on_call_media_state = &on_call_media_state;
    ua_cfg.cb.on_incoming_call = &on_incoming_call;
    ua_cfg.cb.on_reg_state2 = &on_reg_state2;
    ua_cfg.cb.on_call_sdp_created = &on_call_sdp_created;
    ua_cfg.user_agent = pjs("datamodem");

    if (cfg->stun_server[0] != '\0')
    {
        ua_cfg.stun_srv_cnt = 1;
        ua_cfg.stun_srv[0] = pjs(cfg->stun_server);
    }
    if (cfg->nameserver[0] != '\0')
    {
        ua_cfg.nameserver_count = 1;
        ua_cfg.nameserver[0] = pjs(cfg->nameserver);
    }

    pjsua_logging_config_default(&log_cfg);
    log_cfg.console_level = pjsip_level_for(cfg);
    log_cfg.level = pjsip_level_for(cfg);
    log_cfg.msg_logging = dm_log_enabled(DM_LOG_DEBUG) ? PJ_TRUE : PJ_FALSE;
    log_cfg.cb = &dm_log_pjsip_writer;

    pjsua_media_config_default(&med_cfg);
    med_cfg.clock_rate = DM_CLOCK_RATE;
    med_cfg.snd_clock_rate = DM_CLOCK_RATE;
    med_cfg.channel_count = 1;
    med_cfg.audio_frame_ptime = DM_PTIME_MS;
    med_cfg.no_vad = PJ_TRUE; /* silence suppression would cut the carrier */
    med_cfg.ec_tail_len = 0;  /* echo cancellation mangles a modem signal, and
                               * V.22bis in particular relies on the echo of
                               * its own band being left alone */
    med_cfg.quality = 10;     /* no resampling artefacts */
    med_cfg.jb_init = cfg->jitter_buffer_ms;
    med_cfg.jb_min_pre = cfg->jitter_buffer_ms;
    med_cfg.jb_max_pre = cfg->jitter_buffer_ms;
    med_cfg.jb_max = cfg->jitter_buffer_ms * 2;
    /* A jitter buffer that drops or stretches frames to chase latency would
     * corrupt the sample stream the demodulator is tracking; keep every
     * sample, and let the fixed delay be the fixed delay. */
    med_cfg.jb_discard_algo = PJMEDIA_JB_DISCARD_NONE;
    med_cfg.snd_auto_close_time = -1;

    status = pjsua_init(&ua_cfg, &log_cfg, &med_cfg);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_init", status);
        return DM_EXIT_INTERNAL;
    }

    if (strcasecmp(cfg->transport, "tcp") == 0)
        tp_type = PJSIP_TRANSPORT_TCP;
    else if (strcasecmp(cfg->transport, "tls") == 0)
        tp_type = PJSIP_TRANSPORT_TLS;

    pjsua_transport_config_default(&tp_cfg);
    tp_cfg.port = (unsigned) cfg->local_port;
    if (cfg->bind_addr[0] != '\0')
        tp_cfg.bound_addr = pjs(cfg->bind_addr);
    if (cfg->public_addr[0] != '\0')
        tp_cfg.public_addr = pjs(cfg->public_addr);

    status = pjsua_transport_create(tp_type, &tp_cfg, &tp_id);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_transport_create", status);
        return DM_EXIT_SIP;
    }
    {
        /* Log the port actually bound, not the one asked for: with
         * --local-port 0 they differ, and this is the number that has to be
         * reachable. */
        pjsua_transport_info tp_info;
        if (pjsua_transport_get_info(tp_id, &tp_info) == PJ_SUCCESS)
            DM_INFO("sip", "listening on %s %.*s:%d", cfg->transport, (int) tp_info.local_name.host.slen,
                    tp_info.local_name.host.ptr, tp_info.local_name.port);
        else
            DM_INFO("sip", "listening on %s port %d", cfg->transport, cfg->local_port);
    }

    status = pjsua_start();
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_start", status);
        return DM_EXIT_INTERNAL;
    }

    /* There is no sound card in this design: the modem is the endpoint. */
    pjsua_set_null_snd_dev();
    tune_codecs(cfg);

    pjsua_acc_config_default(&acc_cfg);
    if (cfg->from_uri[0] != '\0')
        snprintf(id_uri, sizeof(id_uri), "%s", cfg->from_uri);
    else if (cfg->caller_id[0] != '\0' && strpbrk(cfg->caller_id, "<>\"@:;") == NULL)
        snprintf(id_uri, sizeof(id_uri), "\"%s\" <sip:%s@%s>", cfg->caller_id, cfg->username, cfg->server);
    else
        snprintf(id_uri, sizeof(id_uri), "sip:%s@%s", cfg->username, cfg->server);
    acc_cfg.id = pjs(id_uri);

    if (cfg->do_register)
    {
        snprintf(reg_uri, sizeof(reg_uri), "sip:%s", cfg->server);
        acc_cfg.reg_uri = pjs(reg_uri);
        acc_cfg.reg_timeout = (unsigned) cfg->reg_expires_s;
    }
    if (cfg->proxy[0] != '\0')
    {
        if (strncasecmp(cfg->proxy, "sip:", 4) == 0 || strncasecmp(cfg->proxy, "sips:", 5) == 0)
            snprintf(proxy_uri, sizeof(proxy_uri), "%s", cfg->proxy);
        else
            snprintf(proxy_uri, sizeof(proxy_uri), "sip:%s;lr", cfg->proxy);
        acc_cfg.proxy_cnt = 1;
        acc_cfg.proxy[0] = pjs(proxy_uri);
    }
    if (cfg->password[0] != '\0')
    {
        acc_cfg.cred_count = 1;
        acc_cfg.cred_info[0].realm = pjs(cfg->realm[0] ? cfg->realm : "*");
        acc_cfg.cred_info[0].scheme = pjs("digest");
        acc_cfg.cred_info[0].username = pjs(cfg->auth_user[0] ? cfg->auth_user : cfg->username);
        acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        acc_cfg.cred_info[0].data = pjs(cfg->password);
    }
    if (cfg->rtp_port > 0)
    {
        /* Without a range pjsip walks the port number upward as calls come
         * and go, and eventually negotiates a port outside whatever the
         * firewall or SBC allows - which looks exactly like a carrier that
         * has gone silent. Keep media inside a known window. */
        acc_cfg.rtp_cfg.port = (unsigned) cfg->rtp_port;
        acc_cfg.rtp_cfg.port_range = (unsigned) cfg->rtp_port_range;
        DM_INFO("sip", "media ports %d-%d must be reachable from the far end", cfg->rtp_port,
                cfg->rtp_port + cfg->rtp_port_range);
    }
    acc_cfg.vid_in_auto_show = PJ_FALSE;
    acc_cfg.vid_out_auto_transmit = PJ_FALSE;

    status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g.acc_id);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_acc_add", status);
        return DM_EXIT_SIP;
    }

    if (!cfg->do_register)
    {
        if (acc_cfg.cred_count > 0)
            DM_INFO("sip", "account %s ready, not registering; credentials for %s will answer the "
                           "401/407 challenge on each call",
                    id_uri, cfg->auth_user[0] ? cfg->auth_user : cfg->username);
        else
            DM_INFO("sip", "account %s ready, not registering and no credentials configured; "
                           "the far end must authenticate by source IP",
                    id_uri);
        return DM_EXIT_OK;
    }

    DM_INFO("sip", "registering %s at %s", id_uri, cfg->server);
    dm_term_state("REGISTERING", "%s", id_uri);
    {
        int64_t deadline = dm_now_ms() + (int64_t) cfg->reg_timeout_s * 1000;

        while (!reg_finished() && dm_now_ms() < deadline)
            dm_sip_wait_ms(200);
    }
    if (!reg_finished())
    {
        DM_ERROR("sip", "no answer to REGISTER within %ds", cfg->reg_timeout_s);
        return DM_EXIT_SIP;
    }
    {
        bool ok;

        pthread_mutex_lock(&g.lock);
        ok = g.reg_ok;
        pthread_mutex_unlock(&g.lock);
        return ok ? DM_EXIT_OK : DM_EXIT_SIP;
    }
}

void dm_sip_stop(void)
{
    dm_call_t *c;
    bool hang_up;

    if (!g.started)
        return;
    pthread_mutex_lock(&g.lock);
    c = g.active;
    g.active = NULL;
    g.pending_modem = NULL;
    g.inbound_enabled = false;
    hang_up = (c != NULL && c->call_id != PJSUA_INVALID_ID && !c->disconnected);
    pthread_mutex_unlock(&g.lock);
    if (c != NULL)
    {
        if (hang_up)
            pjsua_call_hangup(c->call_id, 0, NULL, NULL);
        call_destroy(c);
    }
    if (g.acc_id != PJSUA_INVALID_ID && g.cfg.do_register)
        pjsua_acc_set_registration(g.acc_id, PJ_FALSE);
    pjsua_destroy();
    g.started = false;
    DM_INFO("sip", "user agent stopped");
}

/* -------------------------------------------------------------- outbound  */

static void build_request_uri(const dm_config_t *cfg, const char *to, char *out, size_t out_len)
{
    bool is_uri = (strncasecmp(to, "sip:", 4) == 0 || strncasecmp(to, "sips:", 5) == 0);
    const char *transport_param = "";

    if (strcasecmp(cfg->transport, "tcp") == 0)
        transport_param = ";transport=tcp";
    else if (strcasecmp(cfg->transport, "tls") == 0)
        transport_param = ";transport=tls";

    if (is_uri)
        snprintf(out, out_len, "%s", to);
    else
        snprintf(out, out_len, "sip:%s@%s%s%s", to, cfg->server, cfg->user_phone ? ";user=phone" : "",
                 transport_param);
}

/* Builds the P-Asserted-Identity value (RFC 3325) from --caller-id. Anything
 * that already looks like a URI or a name-addr is passed through untouched;
 * a bare number becomes <sip:number@server>, which is what trunks that derive
 * the outbound CLI from this header expect. */
static void build_asserted_identity(const dm_config_t *cfg, char *out, size_t out_len)
{
    const char *cid = cfg->caller_id;

    if (cid[0] == '\0')
    {
        out[0] = '\0';
        return;
    }
    if (cid[0] == '<' || cid[0] == '"')
        snprintf(out, out_len, "%s", cid);
    else if (strncasecmp(cid, "sip:", 4) == 0 || strncasecmp(cid, "sips:", 5) == 0 ||
             strncasecmp(cid, "tel:", 4) == 0)
        snprintf(out, out_len, "<%s>", cid);
    else
        snprintf(out, out_len, "<sip:%s@%s>", cid, cfg->server);
}

int dm_sip_dial(const dm_config_t *cfg, const char *to, dm_modem_t *modem, volatile sig_atomic_t *stop)
{
    pjsua_msg_data msg_data;
    pjsip_generic_string_hdr pai_hdr;
    pj_str_t pai_name = pjs("P-Asserted-Identity");
    pj_str_t pai_value;
    char pai[DM_STR_MAX + 64];
    dm_call_t *c;
    char uri[DM_STR_MAX * 2];
    pj_str_t dst;
    pjsua_call_setting call_cfg;
    pjsua_call_id call_id = PJSUA_INVALID_ID;
    pj_status_t status;
    int64_t deadline;

    if (call_view().exists)
    {
        DM_ERROR("dial", "another call is already in progress");
        return DM_EXIT_INTERNAL;
    }

    build_request_uri(cfg, to, uri, sizeof(uri));
    if (pjsua_verify_sip_url(uri) != PJ_SUCCESS)
    {
        DM_ERROR("dial", "'%s' is not a valid SIP URI", uri);
        return DM_EXIT_CONFIG;
    }

    c = call_create(modem, false);
    if (c == NULL)
        return DM_EXIT_INTERNAL;
    snprintf(c->remote_uri, sizeof(c->remote_uri), "%s", uri);
    snprintf(c->party.remote_uri, sizeof(c->party.remote_uri), "%s", uri);
    dm_parse_sip_party(uri, c->party.remote_name, sizeof(c->party.remote_name), c->party.remote_number,
                       sizeof(c->party.remote_number));
    pthread_mutex_lock(&g.lock);
    g.active = c;
    pthread_mutex_unlock(&g.lock);

    modem_call_setting(&call_cfg);

    /* pjsua clones the header into the INVITE while sending, so these locals
     * only have to outlive pjsua_call_make_call(). */
    pjsua_msg_data_init(&msg_data);
    build_asserted_identity(cfg, pai, sizeof(pai));
    if (pai[0] != '\0')
    {
        pai_value = pjs(pai);
        pjsip_generic_string_hdr_init2(&pai_hdr, &pai_name, &pai_value);
        pj_list_push_back(&msg_data.hdr_list, &pai_hdr);
        DM_DEBUG("dial", "P-Asserted-Identity: %s", pai);
    }

    dst = pjs(uri);
    DM_INFO("dial", "calling %s", uri);
    dm_term_state("DIALING", "%s", to);

    /* pjsua takes c as the call's user data here, and its callbacks can run
     * before this returns; they find c through it, and it is already
     * active. */
    status = pjsua_call_make_call(g.acc_id, &dst, &call_cfg, c, &msg_data, &call_id);
    if (status != PJ_SUCCESS)
    {
        log_pj_error("pjsua_call_make_call", status);
        pthread_mutex_lock(&g.lock);
        g.active = NULL;
        pthread_mutex_unlock(&g.lock);
        call_destroy(c);
        return DM_EXIT_CALL;
    }
    pthread_mutex_lock(&g.lock);
    c->call_id = call_id;
    pthread_mutex_unlock(&g.lock);

    deadline = dm_now_ms() + (int64_t) cfg->connect_timeout_s * 1000;
    for (;;)
    {
        call_view_t v = call_view();

        if (v.media_active && v.answered)
            return DM_EXIT_OK;
        if (v.disconnected)
        {
            int status_code;
            char reason[sizeof(c->last_reason)];

            pthread_mutex_lock(&g.lock);
            status_code = c->last_status;
            snprintf(reason, sizeof(reason), "%s", c->last_reason);
            pthread_mutex_unlock(&g.lock);
            DM_ERROR("dial", "call failed: %d %s", status_code, reason);
            return DM_EXIT_CALL;
        }
        if (dm_now_ms() >= deadline || (stop != NULL && *stop != 0))
            break;
        dm_sip_wait_ms(100);
    }

    if (stop != NULL && *stop != 0)
    {
        /* Ctrl-c while it rings: give up on it now, not when it times out. */
        DM_INFO("dial", "cancelled");
        dm_term_state("CANCELLED", "%s", to);
        pjsua_call_hangup(call_id, PJSIP_SC_REQUEST_TERMINATED, NULL, NULL);
        {
            int64_t grace = dm_now_ms() + DM_HANGUP_GRACE_MS;

            while (!call_view().disconnected && dm_now_ms() < grace)
                dm_sip_wait_ms(100);
        }
        return DM_EXIT_CALL;
    }
    DM_ERROR("dial", "no answer within %ds", cfg->connect_timeout_s);
    pjsua_call_hangup(call_id, PJSIP_SC_REQUEST_TIMEOUT, NULL, NULL);
    {
        int64_t grace = dm_now_ms() + DM_HANGUP_GRACE_MS;

        while (!call_view().disconnected && dm_now_ms() < grace)
            dm_sip_wait_ms(100);
    }
    return DM_EXIT_TIMEOUT;
}

/* --------------------------------------------------------------- inbound  */

int dm_sip_answer(const dm_config_t *cfg, dm_modem_t *modem, volatile sig_atomic_t *stop)
{
    int64_t deadline = cfg->answer_timeout_s > 0
                           ? dm_now_ms() + (int64_t) cfg->answer_timeout_s * 1000
                           : 0;

    pthread_mutex_lock(&g.lock);
    if (g.active != NULL)
    {
        pthread_mutex_unlock(&g.lock);
        DM_ERROR("answer", "another call is already in progress");
        return DM_EXIT_INTERNAL;
    }
    g.pending_modem = modem;
    g.inbound_enabled = true;
    pthread_mutex_unlock(&g.lock);
    DM_INFO("answer", "waiting for an inbound call%s", deadline ? "" : " (ctrl-c to stop)");
    dm_term_state("WAITING", "for a call");

    while (*stop == 0)
    {
        dm_call_t *gone = NULL;
        int status_code = 0;
        char reason[128] = "";
        bool up = false;

        pthread_mutex_lock(&g.lock);
        if (g.active != NULL)
        {
            if (g.active->media_active && g.active->answered)
            {
                up = true;
            }
            else if (g.active->disconnected)
            {
                gone = g.active;
                status_code = gone->last_status;
                snprintf(reason, sizeof(reason), "%s", gone->last_reason);
                g.active = NULL;
                g.pending_modem = modem;
                g.inbound_enabled = true;
            }
        }
        pthread_mutex_unlock(&g.lock);

        if (up)
            return DM_EXIT_OK;
        if (gone != NULL)
        {
            DM_ERROR("answer", "the caller hung up before media came up (%d %s)", status_code, reason);
            call_destroy(gone);
            continue;
        }
        if (deadline != 0 && dm_now_ms() > deadline)
        {
            /* A call claimed in the same instant is left for
             * dm_sip_hangup() and dm_sip_stop() to clear. */
            pthread_mutex_lock(&g.lock);
            g.inbound_enabled = false;
            g.pending_modem = NULL;
            pthread_mutex_unlock(&g.lock);
            DM_ERROR("answer", "no call arrived within %ds", cfg->answer_timeout_s);
            return DM_EXIT_TIMEOUT;
        }
        dm_sip_wait_ms(200);
    }

    pthread_mutex_lock(&g.lock);
    g.inbound_enabled = false;
    g.pending_modem = NULL;
    pthread_mutex_unlock(&g.lock);
    return DM_EXIT_CALL;
}

/* ---------------------------------------------------------------- status  */

bool dm_sip_call_ended(void)
{
    call_view_t v = call_view();

    return !v.exists || v.disconnected;
}

int64_t dm_sip_since_rtp_ms(void)
{
    call_view_t v = call_view();
    pjsua_stream_stat stat;
    int64_t now = dm_now_ms();
    int64_t since = -1;

    if (!v.exists || !v.media_active || v.disconnected || v.call_id == PJSUA_INVALID_ID)
        return -1;
    if (pjsua_call_get_stream_stat(v.call_id, 0, &stat) != PJ_SUCCESS)
        return -1;

    pthread_mutex_lock(&g.lock);
    if (g.active != NULL)
    {
        if (stat.rtcp.rx.pkt != g.active->last_rx_pkts)
        {
            g.active->last_rx_pkts = stat.rtcp.rx.pkt;
            g.active->last_rx_change_ms = now;
        }
        since = now - g.active->last_rx_change_ms;
    }
    pthread_mutex_unlock(&g.lock);
    return since;
}

bool dm_sip_call_party(dm_call_party_t *party)
{
    bool found = false;

    memset(party, 0, sizeof(*party));
    pthread_mutex_lock(&g.lock);
    if (g.active != NULL)
    {
        *party = g.active->party;
        found = true;
    }
    pthread_mutex_unlock(&g.lock);
    return found;
}

bool dm_sip_link_quality(dm_link_quality_t *q)
{
    call_view_t v = call_view();
    pjsua_stream_stat stat;

    memset(q, 0, sizeof(*q));
    if (!v.exists || !v.media_active || v.call_id == PJSUA_INVALID_ID)
        return false;
    if (pjsua_call_get_stream_stat(v.call_id, 0, &stat) != PJ_SUCCESS)
        return false;

    q->rx_packets = stat.rtcp.rx.pkt;
    q->rx_lost = stat.rtcp.rx.loss;
    q->rx_discarded = stat.rtcp.rx.discard;
    q->rx_reordered = stat.rtcp.rx.reorder;
    q->tx_packets = stat.rtcp.tx.pkt;
    q->jitter_us = (unsigned) stat.rtcp.rx.jitter.mean;
    q->jitter_max_us = (unsigned) stat.rtcp.rx.jitter.max;
    return true;
}

void dm_sip_hangup(dm_call_result_t *result)
{
    call_view_t v = call_view();
    dm_call_t *c;

    if (result != NULL)
        memset(result, 0, sizeof(*result));
    if (!v.exists)
        return;

    if (!v.disconnected && v.call_id != PJSUA_INVALID_ID)
    {
        int64_t grace;

        DM_INFO("sip", "clearing call %d", (int) v.call_id);
        pjsua_call_hangup(v.call_id, PJSIP_SC_OK, NULL, NULL);
        grace = dm_now_ms() + DM_HANGUP_GRACE_MS;
        while (!call_view().disconnected && dm_now_ms() < grace)
            dm_sip_wait_ms(100);
    }

    /* Out of g.active first: from here no callback will touch it. */
    pthread_mutex_lock(&g.lock);
    c = g.active;
    g.active = NULL;
    if (c != NULL && result != NULL)
    {
        result->connected = c->media_active;
        result->sip_status = c->last_status;
        snprintf(result->sip_reason, sizeof(result->sip_reason), "%s", c->last_reason);
        snprintf(result->remote_uri, sizeof(result->remote_uri), "%s", c->remote_uri);
        result->duration_ms =
            (int) (dm_now_ms() - (c->answered_ms ? c->answered_ms : c->started_ms));
    }
    pthread_mutex_unlock(&g.lock);
    call_destroy(c);
}
