/* `datamodem modem`: a Hayes modem on a serial port, which is what a terminal
 * program - SyncTERM's Modem connection, Telix, minicom - expects to find at
 * the end of one. AT commands come in on the port and result codes go back;
 * ATD places a SIP call and the session runs on the port; DCD follows the
 * carrier, and dropping DTR hangs up.
 *
 * The control lines go the way a null-modem cable, and com0com's default
 * wiring, take them: our DTR is the terminal's DCD and DSR, and its DTR is
 * our DSR. */
#ifndef DATAMODEM_HAYES_H
#define DATAMODEM_HAYES_H

#include <signal.h>

#include "datamodem/config.h"

/* Opens cfg->port and answers AT commands on it, call after call, until
 * *stop. Returns a dm_exit_code_t. */
int dm_hayes_run(const dm_config_t *cfg, volatile sig_atomic_t *stop);

/* The result code for a call that never connected, from what dm_sip_start()
 * or dm_sip_dial() returned and the call's final SIP status (0 for none):
 * NO DIALTONE, BUSY, NO ANSWER or NO CARRIER. */
const char *dm_hayes_dial_failure(int rc, int sip_status);

#endif /* DATAMODEM_HAYES_H */
