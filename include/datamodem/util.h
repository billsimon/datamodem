#ifndef DATAMODEM_UTIL_H
#define DATAMODEM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Monotonic milliseconds, for deadlines and the escape guard timer. */
int64_t dm_now_ms(void);

/* The clock without any simulated offset, for measuring how long something
 * really took. */
int64_t dm_now_real_ms(void);

/* Moves dm_now_ms() forward. For the selftest, which generates audio far
 * faster than real time and would otherwise see a frozen clock. Nothing on a
 * real call touches this. */
void dm_clock_advance_ms(int64_t ms);

/* mkdir -p on the directory part of a file path. Returns false and fills err
 * on failure. */
bool dm_mkdir_parent(const char *path, char *err, size_t err_len);

/* "1h02m03s", for the session summary. */
void dm_format_duration(int64_t ms, char *out, size_t out_len);

/* Renders one byte the way a terminal log should show it: printable ASCII as
 * itself, everything else as <XX>. out needs 8 bytes. */
void dm_format_byte(int c, char *out, size_t out_len);

/* Parses a control-character spec for --escape-key: "^]" / "0x1d" / "29" /
 * "none". Returns false on a bad spec; *out is -1 for "none". */
bool dm_parse_key_spec(const char *s, int *out);

#endif /* DATAMODEM_UTIL_H */
