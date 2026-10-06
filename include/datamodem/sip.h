/* SIP user agent: registration, call setup, and the bridge between the RTP
 * audio stream and the softmodem. G.711 audio only - the modem lives in the
 * audio band, so there is no T.38-style relay to fall back on. */
#ifndef DATAMODEM_SIP_H
#define DATAMODEM_SIP_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "datamodem/config.h"
#include "datamodem/modem.h"

typedef struct
{
    bool connected;        /* media came up */
    int sip_status;        /* final SIP status code */
    char sip_reason[128];
    char remote_uri[256];  /* who we ended up talking to */
    int duration_ms;       /* from INVITE (or answer) to disconnect */
} dm_call_result_t;

/* Brings up pjsua, the transport and the account. Returns a dm_exit_code_t. */
int dm_sip_start(const dm_config_t *cfg);
void dm_sip_stop(void);

/* Places a call to `to` and waits for media. On DM_EXIT_OK the modem is
 * attached to the audio path and audio is already flowing; the caller then
 * runs the session and calls dm_sip_hangup() when done. Any other return
 * means there is nothing to run and the call has been cleared. */
int dm_sip_dial(const dm_config_t *cfg, const char *to, dm_modem_t *modem, volatile sig_atomic_t *stop);

/* Answers one inbound call, attaching `modem` to it. Waits up to
 * cfg->answer_timeout_s (0 = forever) or until *stop becomes non-zero. */
int dm_sip_answer(const dm_config_t *cfg, dm_modem_t *modem, volatile sig_atomic_t *stop);

/* True once the far end has cleared, or the stack has torn the call down. */
bool dm_sip_call_ended(void);

/* Time since an RTP packet last arrived, or -1 when the watchdog cannot
 * sample the stream yet. */
int64_t dm_sip_since_rtp_ms(void);

/* How the audio path is actually behaving. A modem is far less forgiving of
 * a lossy or jittery path than speech is, and when a call trains below its
 * offered rate, retrains repeatedly, or delivers noise, this is the first
 * thing worth looking at. Returns false if the stream cannot be sampled. */
typedef struct
{
    unsigned rx_packets;
    unsigned rx_lost;
    unsigned rx_discarded;
    unsigned rx_reordered;
    unsigned tx_packets;
    unsigned jitter_us;        /* mean inter-arrival jitter */
    unsigned jitter_max_us;
} dm_link_quality_t;

bool dm_sip_link_quality(dm_link_quality_t *q);

/* Who the call is with. On an answered call, remote_number and remote_name
 * are the caller ID - from P-Asserted-Identity when the trunk sends it,
 * otherwise From - and local_number is the number that was called. On a
 * placed call, remote_number is the number dialled. */
typedef struct
{
    char remote_uri[256];
    char remote_name[128];
    char remote_number[128];
    char local_number[128];
} dm_call_party_t;

/* Fills party for the call in progress. Returns false, with party empty,
 * when there is none. */
bool dm_sip_call_party(dm_call_party_t *party);

/* Clears the call and waits briefly for the BYE to be acknowledged. */
void dm_sip_hangup(dm_call_result_t *result);

/* Blocks the calling thread for up to ms, returning early when anything about
 * the call changes. The session loop uses this only when it has no terminal
 * to poll. */
void dm_sip_wait_ms(int ms);

#endif /* DATAMODEM_SIP_H */
