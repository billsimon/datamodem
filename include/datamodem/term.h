/* The screen, the way a 1990s terminal program had it: the far end's output
 * fills the screen above a status line that says what the modem is doing,
 * and nothing the far end sends can reach the terminal itself.
 *
 * That last part is the point of the design. A BBS sends ANSI - colour,
 * cursor movement, screen clears - and line noise or a binary file sends
 * anything at all. Passing it to the terminal would let it switch character
 * sets, change modes, set the title, move the scroll region or overwrite
 * the status line, and some of that outlives the program. So the far end's
 * bytes drive an emulated screen (ANSI-BBS, as ANSI.SYS and the BBS
 * terminals understood it), and only what this module generates itself is
 * ever written to the real one.
 *
 * Eight-bit characters are the far end's code page: CP437 by default, as
 * every PC BBS assumed, translated to Unicode for the terminal.
 *
 * Only for an interactive session: when stdout is not a terminal, output is
 * passed through untouched as before, so transcripts and pipes still get
 * exactly what came off the line. */
#ifndef DATAMODEM_TERM_H
#define DATAMODEM_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "datamodem/config.h"
#include "datamodem/modem.h"

/* Take over the screen: clear it, put the status line on the bottom row.
 * Does nothing, and returns false, unless stdin and stdout are both
 * terminals, --tui is on and the terminal is big enough. */
bool dm_term_start(const dm_config_t *cfg);

/* Give the screen back: the status line stays where it is, the shell
 * continues on a new line below it, and any errors that were kept off the
 * screen while it was ours are printed there. Safe to call more than once,
 * and when the screen was never taken. */
void dm_term_stop(void);

bool dm_term_active(void);

/* Where answers to the far end's terminal queries (its cursor position
 * report, mainly, which BBSes use to detect ANSI) are sent. */
void dm_term_set_reply(void (*reply)(void *user, const void *data, size_t len), void *user);

/* The far end's bytes, through the emulator. Main thread. */
void dm_term_remote(const void *data, size_t len);

/* Our own text - result codes, the AT conversation - written into the same
 * screen as plain text. Main thread. */
void dm_term_local(const void *text, size_t len);

/* The status line's state: a short label (DIALING, ONLINE, ...) and a
 * detail. Any thread; drawn by the next dm_term_tick(). */
void dm_term_state(const char *label, const char *detail_fmt, ...) __attribute__((format(printf, 2, 3)));

/* What the link is doing, for the status line. Main thread. connected_ms
 * is when it came up, 0 if it has not. */
void dm_term_link(const dm_modem_status_t *st, int64_t connected_ms, bool command_mode);

/* The call is over: the status line says so, with its clock stopped. */
void dm_term_end(const char *why);

/* Make sure what is written next starts a line of its own - a result code
 * after whatever noise the far end left half a line of, as a modem's own
 * CR LF before NO CARRIER did. */
void dm_term_fresh_line(void);

/* Redraw the status line if anything on it changed, and follow a window
 * resize. Main thread, often. */
void dm_term_tick(void);

/* When the screen is not ours but stdout is still a terminal (--no-tui, or
 * input from a pipe): the far end's bytes made safe to print in line -
 * printable text, CR, LF, backspace, tab, colour and the movements that stay
 * on the current line (cursor forward, back, to a column, erase in line),
 * its code page translated - and nothing else. Queries are answered through
 * dm_term_set_reply()'s hook, as the emulator answers them, for a screen the
 * size of the terminal. Returns the length written to out, which
 * must hold DM_TERM_SANITIZE_CAP(len) bytes. */
#define DM_TERM_SANITIZE_CAP(len) (4 * (len) + 80)
size_t dm_term_sanitize(const unsigned char *in, size_t len, char *out);

/* Undo anything dm_term_sanitize let through that outlasts a line (colour). */
void dm_term_sanitize_end(void);

#endif /* DATAMODEM_TERM_H */
