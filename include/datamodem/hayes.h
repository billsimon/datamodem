/* `datamodem modem`: a Hayes modem on a serial port, which is what a terminal
 * program - SyncTERM's Modem connection, Telix, minicom - expects to find at
 * the end of one. AT commands come in on the port and result codes go back;
 * ATD places a SIP call and the session runs on the port; DCD follows the
 * carrier, and dropping DTR hangs up.
 *
 * The control lines go the way a null-modem cable takes them: our DTR is the
 * terminal's DCD and DSR, and its DTR is our DSR. */
#ifndef DATAMODEM_HAYES_H
#define DATAMODEM_HAYES_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#include "datamodem/config.h"

/* Opens cfg->port and answers AT commands on it, call after call, until
 * *stop. Returns a dm_exit_code_t. */
int dm_hayes_run(const dm_config_t *cfg, volatile sig_atomic_t *stop);

/* The result code for a call that never connected, from what dm_sip_start()
 * or dm_sip_dial() returned and the call's final SIP status (0 for none):
 * NO DIALTONE, BUSY, NO ANSWER or NO CARRIER. */
const char *dm_hayes_dial_failure(int rc, int sip_status);

/* A number to dial, from the way a dialling directory has it - "1 (555)
 * 123-4567", "9W5551234" - or a SIP URI as it stands. What made sense to a
 * telephone exchange - tone or pulse, wait for dial tone, pause, flash - means
 * nothing to a SIP trunk and is dropped. False when nothing is left. */
bool dm_hayes_number(const char *dial, char *out, size_t out_len);

#endif /* DATAMODEM_HAYES_H */
