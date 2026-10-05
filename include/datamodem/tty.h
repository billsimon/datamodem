/* Terminal handling.
 *
 * While a call is up the local terminal has to behave like the serial port a
 * modem used to hang off: every keystroke goes out on the line unaltered, and
 * every byte off the line lands on the screen unaltered. That means raw mode
 * with ISIG and OPOST off, so ctrl-C reaches the remote host instead of
 * killing us and the remote's own CR/LF is not rewritten on the way out.
 *
 * The consequence is that the escape sequence is the only local way out, so
 * restoring the terminal is handled defensively: on normal exit, on a signal,
 * and via atexit. */
#ifndef DATAMODEM_TTY_H
#define DATAMODEM_TTY_H

#include <stdbool.h>
#include <stddef.h>

/* True when stdin is a terminal. When it is not (a pipe, a here-doc, a script)
 * raw mode is skipped and the session runs unattended. */
bool dm_tty_is_interactive(void);

/* Puts stdin into raw mode and registers the restore path. A no-op, returning
 * true, when stdin is not a terminal. */
bool dm_tty_raw(char *err, size_t err_len);

/* Puts the terminal back. Safe to call more than once, and from a signal
 * handler. */
void dm_tty_restore(void);

/* Echo and line editing off, signals left on: for while a call is being set
 * up with the screen ours, so that typing does not land on it and ctrl-c
 * still quits. dm_tty_restore() undoes it. */
void dm_tty_quiet(void);

/* Straight to stdout, for escape sequences of our own. */
bool dm_tty_write_raw(const void *data, size_t len);

/* Writes data that came off the line. Always stdout, retrying short writes,
 * so `datamodem 5551234 > session.txt` captures exactly what the remote
 * system sent and nothing else. Returns false if the descriptor is gone. */
bool dm_tty_write(const void *data, size_t len);

/* Writes local output: the result codes, the AT conversation, the echo of
 * what you typed. None of it came off the line, so it goes to stdout only
 * when stdout is the terminal you are sitting at; when stdout has been
 * redirected it follows you to stderr instead. */
bool dm_tty_control(const void *data, size_t len);

/* A result code or a status line, CRLF-terminated because OPOST is off
 * while raw. Goes to the control stream. */
void dm_tty_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* DATAMODEM_TTY_H */
