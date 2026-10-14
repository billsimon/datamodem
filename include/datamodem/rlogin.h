/* `datamodem rlogin-listen`: takes a terminal program's RLogin connections -
 * SyncTERM's RLogin connection type - and dials the number each one carries
 * in its user name. Once the modems have trained the connection is the line,
 * byte for byte, until one end or the other hangs up; closing the connection
 * is hanging up.
 *
 * This is what a terminal program on Windows uses in place of --stdio, which
 * SyncTERM's Shell connection cannot carry there, and with no virtual serial
 * port to install. */
#ifndef DATAMODEM_RLOGIN_H
#define DATAMODEM_RLOGIN_H

#include <signal.h>

#include "datamodem/config.h"

/* Listens on cfg->rlogin_bind and cfg->rlogin_port and takes connections
 * one at a time, a call each, until *stop. A connection that arrives while
 * a call is up is told so and closed. Returns a dm_exit_code_t. */
int dm_rlogin_run(const dm_config_t *cfg, volatile sig_atomic_t *stop);

#endif /* DATAMODEM_RLOGIN_H */
