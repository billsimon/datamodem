/* The interactive session: the loop that joins the terminal to the modem
 * once a call is up, the Hayes escape detector that gets you back out, and
 * the small AT interpreter that runs while you are out. */
#ifndef DATAMODEM_SESSION_H
#define DATAMODEM_SESSION_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "datamodem/config.h"
#include "datamodem/modem.h"

typedef enum
{
    DM_END_HANGUP = 0,      /* the user asked to hang up (ATH, or EOF on a pipe) */
    DM_END_NO_CARRIER,      /* the far modem dropped carrier */
    DM_END_REMOTE_CLEARED,  /* the SIP call went away under us */
    DM_END_TRAIN_FAILED,    /* the modems never trained */
    DM_END_IDLE,            /* idle timeout */
    DM_END_UNSTABLE,        /* the carrier kept dropping and retraining */
    DM_END_TIMEOUT,         /* max call length or media watchdog */
    DM_END_SIGNAL,          /* SIGINT/SIGTERM */
    DM_END_ERROR
} dm_session_end_t;

typedef struct
{
    dm_session_end_t reason;
    int64_t duration_ms;       /* whole session, including training */
    int64_t connected_ms;      /* time spent with carrier up */
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    int bit_rate;
} dm_session_result_t;

const char *dm_session_end_name(dm_session_end_t reason);

/* Runs the session until it ends. The call must already be up and the modem
 * attached to it. Returns a dm_exit_code_t and always fills result. */
int dm_session_run(const dm_config_t *cfg, dm_modem_t *modem, volatile sig_atomic_t *stop,
                   dm_session_result_t *result);

/* ----------------------------------------------------------- escape detector
 *
 * The Hayes rule, implemented on the bytes on their way out to the line:
 *
 *   1. at least S12 (--escape-guard-ms) of no data from the terminal, then
 *   2. the escape character (S2, --escape-char) three times, each within S12
 *      of the last and with nothing else in between, then
 *   3. at least S12 of no data again.
 *
 * The guard time either side is what stops a file that happens to contain
 * "+++" from dropping you into command mode mid-transfer. The pluses are held
 * back while the sequence is in progress and released to the line if it turns
 * out not to be one, which is why this sits in the transmit path rather than
 * watching a copy. */

typedef struct
{
    int escape_char;        /* >127 disables the detector entirely */
    int guard_ms;
    int64_t last_data_ms;   /* when a non-escape byte last went out */
    int64_t last_plus_ms;   /* when the most recent held escape char arrived */
    int count;              /* escape characters held so far, 0..3 */
    bool armed;             /* the pre-guard has been satisfied */
    bool tripped;           /* the post-guard has elapsed: escape to command mode */
} dm_escape_t;

void dm_escape_init(dm_escape_t *e, int escape_char, int guard_ms);

/* Feeds one byte from the terminal. Returns the number of bytes to actually
 * transmit, written into out (which needs 4 bytes): normally 1, zero while
 * escape characters are being held, and up to 4 when held characters are
 * released along with the byte that broke the sequence. */
int dm_escape_feed(dm_escape_t *e, int64_t now_ms, unsigned char c, unsigned char *out);

/* Call when the terminal is quiet. Returns true once the trailing guard time
 * has passed with three characters held: the session should switch to command
 * mode. Also releases held characters, into out, when the sequence lapses. */
bool dm_escape_tick(dm_escape_t *e, int64_t now_ms, unsigned char *out, int *out_len);

#endif /* DATAMODEM_SESSION_H */
