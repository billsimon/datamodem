#ifndef DATAMODEM_ALSA_SHIM_H
#define DATAMODEM_ALSA_SHIM_H

#include <stdbool.h>

/* Linux release builds only (DATAMODEM_ALSA_SHIM): load libasound.so.2 so
 * that pjmedia can find a sound device. Until this is called, and if it
 * fails, ALSA reports no devices. Call it, if at all, before pjsua starts.
 * False when libasound is not installed. */
bool dm_alsa_load(void);

#endif /* DATAMODEM_ALSA_SHIM_H */
