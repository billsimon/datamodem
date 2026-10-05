#include "datamodem/term.h"
#include "datamodem/log.h"
#include "datamodem/tty.h"
#include "datamodem/util.h"
#include "datamodem/version.h"

#include <errno.h>
#include <inttypes.h>
#include <langinfo.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

/* ------------------------------------------------------------- charsets */

enum
{
    CS_CP437 = 0,
    CS_UTF8,
    CS_ASCII
};

/* Code page 437, 0x80-0xFF: what every PC BBS drew its boxes and shading
 * with. */
static const uint16_t CP437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static int g_charset = CS_CP437;
static bool g_out_utf8 = true;      /* the terminal takes UTF-8 */

static int charset_parse(const char *s)
{
    if (s != NULL && (strcasecmp(s, "utf8") == 0 || strcasecmp(s, "utf-8") == 0))
        return CS_UTF8;
    if (s != NULL && (strcasecmp(s, "ascii") == 0 || strcasecmp(s, "us-ascii") == 0))
        return CS_ASCII;
    return CS_CP437;
}

static const char *charset_name(int cs)
{
    return cs == CS_UTF8 ? "UTF-8" : (cs == CS_ASCII ? "ASCII" : "CP437");
}

/* One code point, as the terminal should be sent it: UTF-8, or for a
 * terminal that does not take it, the nearest ASCII. */
static size_t encode(uint32_t cp, char *out)
{
    if (cp < 0x80)
    {
        out[0] = (char) cp;
        return 1;
    }
    if (!g_out_utf8)
    {
        if (cp >= 0x2500 && cp <= 0x257F)
        {
            /* Box drawing: horizontals, verticals, and everything else is
             * a corner or a junction. */
            static const uint32_t horiz[] = { 0x2500, 0x2501, 0x2550, 0x254C, 0x254D };
            static const uint32_t vert[] = { 0x2502, 0x2503, 0x2551, 0x254E, 0x254F };

            for (size_t i = 0; i < sizeof(horiz) / sizeof(horiz[0]); i++)
                if (cp == horiz[i])
                    return (size_t) (out[0] = '-', 1);
            for (size_t i = 0; i < sizeof(vert) / sizeof(vert[0]); i++)
                if (cp == vert[i])
                    return (size_t) (out[0] = '|', 1);
            out[0] = '+';
            return 1;
        }
        out[0] = (cp >= 0x2580 && cp <= 0x259F) ? '#' : '?';
        return 1;
    }
    if (cp < 0x800)
    {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char) (0xF0 | (cp >> 18));
    out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

/* A decoder for one byte stream in the far end's code page: feed bytes,
 * get code points. -1 means nothing yet. */
typedef struct
{
    uint32_t cp;
    int need;
} decoder_t;

static int32_t decode(decoder_t *d, int cs, unsigned char b)
{
    if (cs == CS_CP437)
        return (b < 0x80) ? b : CP437[b - 0x80];
    if (cs == CS_ASCII)
        return (b < 0x80) ? b : '?';

    if (d->need > 0)
    {
        if ((b & 0xC0) == 0x80)
        {
            d->cp = (d->cp << 6) | (b & 0x3F);
            if (--d->need == 0)
            {
                /* C1 controls are not text, and are not passed on. */
                if (d->cp >= 0x80 && d->cp < 0xA0)
                    return -1;
                if (d->cp > 0x10FFFF || (d->cp >= 0xD800 && d->cp < 0xE000))
                    return 0xFFFD;
                return (int32_t) d->cp;
            }
            return -1;
        }
        /* Broken sequence: say so, and start again with this byte. */
        d->need = 0;
        if (b < 0x80)
            return b; /* the replacement for what came before is lost; the byte is not */
    }
    if (b < 0x80)
        return b;
    if ((b & 0xE0) == 0xC0)
    {
        d->cp = b & 0x1F;
        d->need = 1;
        return -1;
    }
    if ((b & 0xF0) == 0xE0)
    {
        d->cp = b & 0x0F;
        d->need = 2;
        return -1;
    }
    if ((b & 0xF8) == 0xF0)
    {
        d->cp = b & 0x07;
        d->need = 3;
        return -1;
    }
    return 0xFFFD;
}

static int cell_width(uint32_t cp)
{
    int w;

    if (cp < 0x300)
        return 1;
    w = wcwidth((wchar_t) cp);
    if (w < 0)
        return 1;
    return (w > 2) ? 2 : w;
}

/* ------------------------------------------------------------- the screen */

#define A_BOLD 0x01
#define A_DIM 0x02
#define A_ITALIC 0x04
#define A_UNDER 0x08
#define A_BLINK 0x10
#define A_REVERSE 0x20
#define A_HIDDEN 0x40
#define COLOR_DEFAULT 0xFFFF
#define CH_WIDE_TAIL 0xFFFFFFFEu /* the right half of a double-width character */
#define CH_UNKNOWN 0xFFFFFFFFu   /* what is on the real screen is not known */

typedef struct
{
    uint32_t ch;
    uint16_t fg;
    uint16_t bg;
    uint8_t attr;
} cell_t;

typedef struct
{
    uint16_t fg;
    uint16_t bg;
    uint8_t attr;
} pen_t;

enum
{
    P_GROUND = 0,
    P_ESC,
    P_ESC_SKIP1,   /* ESC ( x and friends: one more byte, ignored */
    P_CSI,
    P_STRING,      /* OSC, DCS, APC, PM, SOS: up to BEL or ST, ignored */
    P_STRING_ESC
};

#define MAX_PARAMS 16
#define OUT_CAP 65536

static struct
{
    bool active;
    int W, H;
    int rows, cols;             /* the emulated screen: everything above the status line */
    cell_t *cells;              /* what the far end has drawn */
    cell_t *shown;              /* what the real screen is showing */
    int pending_scroll;         /* whole-screen scrolls to do on the real one first */

    int cx, cy;
    bool wrap_pending;
    bool autowrap;
    bool cursor_visible;
    pen_t pen;
    int top, bot;               /* scrolling margins, 0-based, inclusive */
    int sx, sy;
    pen_t spen;

    int pstate;
    int params[MAX_PARAMS];
    int nparams;
    bool param_started;
    char priv;
    char inter;
    int string_len;
    decoder_t dec;
    decoder_t local_dec;

    /* The real terminal, as last left. */
    char out[OUT_CAP];
    size_t olen;
    int px, py;                 /* -1 = not known */
    pen_t ppen;
    bool ppen_known;
    bool pcursor_visible;
    bool bell;
    int64_t last_bell_ms;

    void (*reply)(void *user, const void *data, size_t len);
    void *reply_user;

    /* The status line. Written by any thread under lock, drawn by the main
     * thread. */
    pthread_mutex_t lock;
    char label[24];
    char detail[160];
    char notice[200];
    int64_t notice_until;
    char errors[6][240];
    int nerrors;
    char rate[64];              /* "14400 V.42/V.42bis" */
    char snr[24];
    char traffic[48];
    int64_t since_ms;
    int64_t connected_ms;
    int64_t ended_ms;           /* the clock stops here, 0 while it runs */
    bool command_mode;
    char drawn_status[512];

    struct sigaction old_winch;
    bool stopped;
} T = { .lock = PTHREAD_MUTEX_INITIALIZER };

static volatile sig_atomic_t g_winch = 0;

static void on_winch(int sig)
{
    (void) sig;
    g_winch = 1;
}

static cell_t blank_cell(const pen_t *p)
{
    cell_t c = { ' ', COLOR_DEFAULT, p ? p->bg : COLOR_DEFAULT, 0 };

    return c;
}

static cell_t *cell_at(int x, int y)
{
    return &T.cells[y * T.cols + x];
}

/* ------------------------------------------------------------ output */

static void out_flush(void)
{
    size_t off = 0;

    while (off < T.olen)
    {
        ssize_t n = write(STDOUT_FILENO, T.out + off, T.olen - off);

        if (n > 0)
            off += (size_t) n;
        else if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        else
            break;
    }
    T.olen = 0;
}

static void out_bytes(const char *s, size_t n)
{
    if (T.olen + n > OUT_CAP)
        out_flush();
    if (n > OUT_CAP)
        return;
    memcpy(T.out + T.olen, s, n);
    T.olen += n;
}

static void out_str(const char *s)
{
    out_bytes(s, strlen(s));
}

static void out_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out_fmt(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        out_bytes(buf, (size_t) n < sizeof(buf) ? (size_t) n : sizeof(buf) - 1);
}

static void out_move(int x, int y)
{
    if (T.px == x && T.py == y)
        return;
    out_fmt("\033[%d;%dH", y + 1, x + 1);
    T.px = x;
    T.py = y;
}

static void color_params(char *buf, size_t len, uint16_t c, bool bg)
{
    if (c < 8)
        snprintf(buf, len, ";%d", (bg ? 40 : 30) + c);
    else if (c < 16)
        snprintf(buf, len, ";%d", (bg ? 100 : 90) + c - 8);
    else
        snprintf(buf, len, ";%d;5;%d", bg ? 48 : 38, c);
}

static void out_pen(const pen_t *p)
{
    char seq[96];
    char col[24];
    size_t o;

    if (T.ppen_known && T.ppen.fg == p->fg && T.ppen.bg == p->bg && T.ppen.attr == p->attr)
        return;
    o = (size_t) snprintf(seq, sizeof(seq), "\033[0");
    if (p->attr & A_BOLD)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";1");
    if (p->attr & A_DIM)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";2");
    if (p->attr & A_ITALIC)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";3");
    if (p->attr & A_UNDER)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";4");
    if (p->attr & A_BLINK)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";5");
    if (p->attr & A_REVERSE)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";7");
    if (p->attr & A_HIDDEN)
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, ";8");
    if (p->fg != COLOR_DEFAULT)
    {
        color_params(col, sizeof(col), p->fg, false);
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, "%s", col);
    }
    if (p->bg != COLOR_DEFAULT)
    {
        color_params(col, sizeof(col), p->bg, true);
        o += (size_t) snprintf(seq + o, sizeof(seq) - o, "%s", col);
    }
    snprintf(seq + o, sizeof(seq) - o, "m");
    out_str(seq);
    T.ppen = *p;
    T.ppen_known = true;
}

/* ------------------------------------------------------------ rendering */

static bool cell_eq(const cell_t *a, const cell_t *b)
{
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

static void invalidate_shown(void)
{
    for (int i = 0; i < T.rows * T.cols; i++)
        T.shown[i].ch = CH_UNKNOWN;
    T.pending_scroll = 0;
}

static void draw_status(bool force);

/* Bring the real screen into line with the emulated one: the cheapest way to
 * a screen that matches, whatever the far end did to get there. */
static void render(void)
{
    if (!T.active)
        return;

    if (T.pending_scroll > 0)
    {
        pen_t plain = { COLOR_DEFAULT, COLOR_DEFAULT, 0 };

        /* A line feed on the bottom line scrolls the region, and only the
         * region: the status line stays put. */
        out_pen(&plain);
        out_move(0, T.rows - 1);
        for (int i = 0; i < T.pending_scroll; i++)
            out_str("\n");
        T.pending_scroll = 0;
    }

    for (int y = 0; y < T.rows; y++)
    {
        for (int x = 0; x < T.cols; x++)
        {
            cell_t *c = cell_at(x, y);
            cell_t *s = &T.shown[y * T.cols + x];
            char enc[4];
            pen_t p;
            int w;

            if (cell_eq(c, s))
            {
                /* A short run of cells already right, followed by one that
                 * is not, is cheaper rewritten than skipped with a cursor
                 * movement - and keeps words in one piece on the wire. */
                int gap = 0;

                if (T.py == y && T.px == x)
                {
                    while (x + gap + 1 < T.cols && gap <= 6 &&
                           cell_eq(cell_at(x + gap, y), &T.shown[y * T.cols + x + gap]) &&
                           cell_at(x + gap, y)->ch != CH_WIDE_TAIL && cell_at(x + gap + 1, y)->ch != CH_WIDE_TAIL)
                        gap++;
                    /* Only for a run that really ends at a cell to draw. */
                    if (gap > 0 && gap <= 6 && x + gap < T.cols &&
                        !cell_eq(cell_at(x + gap, y), &T.shown[y * T.cols + x + gap]))
                    {
                        for (int k = 0; k < gap; k++)
                        {
                            cell_t *g = cell_at(x + k, y);
                            pen_t gp = { g->fg, g->bg, g->attr };
                            char genc[4];

                            out_pen(&gp);
                            out_bytes(genc, encode(g->ch, genc));
                        }
                        T.px += gap;
                        x += gap - 1;
                    }
                }
                continue;
            }
            if (c->ch == CH_WIDE_TAIL)
            {
                *s = *c;
                continue;
            }
            w = (x + 1 < T.cols && T.cells[y * T.cols + x + 1].ch == CH_WIDE_TAIL) ? 2 : 1;
            out_move(x, y);
            p.fg = c->fg;
            p.bg = c->bg;
            p.attr = c->attr;
            out_pen(&p);
            out_bytes(enc, encode(c->ch, enc));
            *s = *c;
            T.px += w;
            if (T.px >= T.cols)
                T.px = -1; /* wherever deferred wrap has left it */
        }
    }

    if (T.bell)
    {
        int64_t now = dm_now_ms();

        /* A file full of 0x07 should not be a minute of beeping. */
        if (now - T.last_bell_ms > 500)
        {
            out_str("\a");
            T.last_bell_ms = now;
        }
        T.bell = false;
    }
    draw_status(false);

    if (T.cursor_visible != T.pcursor_visible)
    {
        out_str(T.cursor_visible ? "\033[?25h" : "\033[?25l");
        T.pcursor_visible = T.cursor_visible;
    }
    out_move(T.cx, T.cy);
    out_flush();
}

/* ------------------------------------------------------------ emulation */

static void scroll_up(int top, int bot, int n)
{
    int h = bot - top + 1;

    if (n <= 0)
        return;
    if (n > h)
        n = h;
    memmove(&T.cells[top * T.cols], &T.cells[(top + n) * T.cols], sizeof(cell_t) * (size_t) ((h - n) * T.cols));
    for (int y = bot - n + 1; y <= bot; y++)
        for (int x = 0; x < T.cols; x++)
            *cell_at(x, y) = blank_cell(&T.pen);

    /* The whole screen scrolling is what a terminal session mostly does;
     * the real screen can do that itself, with the shown copy following. */
    if (top == 0 && bot == T.rows - 1)
    {
        memmove(&T.shown[0], &T.shown[n * T.cols], sizeof(cell_t) * (size_t) ((T.rows - n) * T.cols));
        for (int i = (T.rows - n) * T.cols; i < T.rows * T.cols; i++)
        {
            T.shown[i].ch = ' ';
            T.shown[i].fg = COLOR_DEFAULT;
            T.shown[i].bg = COLOR_DEFAULT;
            T.shown[i].attr = 0;
        }
        T.pending_scroll += n;
        if (T.pending_scroll >= T.rows)
            invalidate_shown();
    }
}

static void scroll_down(int top, int bot, int n)
{
    int h = bot - top + 1;

    if (n <= 0)
        return;
    if (n > h)
        n = h;
    memmove(&T.cells[(top + n) * T.cols], &T.cells[top * T.cols], sizeof(cell_t) * (size_t) ((h - n) * T.cols));
    for (int y = top; y < top + n; y++)
        for (int x = 0; x < T.cols; x++)
            *cell_at(x, y) = blank_cell(&T.pen);
}

static void erase(int x0, int y0, int x1, int y1)
{
    /* Inclusive, in reading order. */
    for (int y = y0; y <= y1; y++)
    {
        int a = (y == y0) ? x0 : 0;
        int b = (y == y1) ? x1 : T.cols - 1;

        for (int x = a; x <= b; x++)
            *cell_at(x, y) = blank_cell(&T.pen);
    }
}

static void linefeed(void)
{
    if (T.cy == T.bot)
        scroll_up(T.top, T.bot, 1);
    else if (T.cy < T.rows - 1)
        T.cy++;
}

static void reverse_index(void)
{
    if (T.cy == T.top)
        scroll_down(T.top, T.bot, 1);
    else if (T.cy > 0)
        T.cy--;
}

static void clear_screen(void)
{
    erase(0, 0, T.cols - 1, T.rows - 1);
    T.cx = T.cy = 0;
    T.wrap_pending = false;
}

static void put_char(uint32_t cp)
{
    int w = cell_width(cp);
    cell_t *c;

    if (w == 0)
        return; /* combining marks: not worth a model of their own */
    if (T.wrap_pending)
    {
        T.wrap_pending = false;
        T.cx = 0;
        linefeed();
    }
    if (w == 2 && T.cx == T.cols - 1)
    {
        if (!T.autowrap)
            return;
        *cell_at(T.cx, T.cy) = blank_cell(&T.pen);
        T.cx = 0;
        linefeed();
    }
    /* Overwriting half of a double-width character leaves the other half
     * meaningless. */
    c = cell_at(T.cx, T.cy);
    if (c->ch == CH_WIDE_TAIL && T.cx > 0)
        *cell_at(T.cx - 1, T.cy) = blank_cell(&T.pen);
    if (T.cx + w < T.cols && cell_at(T.cx + w, T.cy)->ch == CH_WIDE_TAIL)
        *cell_at(T.cx + w, T.cy) = blank_cell(&T.pen);

    c->ch = cp;
    c->fg = T.pen.fg;
    c->bg = T.pen.bg;
    c->attr = T.pen.attr;
    if (w == 2)
    {
        cell_t *t = cell_at(T.cx + 1, T.cy);

        *t = *c;
        t->ch = CH_WIDE_TAIL;
    }
    if (T.cx + w >= T.cols)
    {
        T.cx = T.cols - 1;
        T.wrap_pending = T.autowrap;
    }
    else
    {
        T.cx += w;
    }
}

static void reset_state(void)
{
    T.pen.fg = T.pen.bg = COLOR_DEFAULT;
    T.pen.attr = 0;
    T.top = 0;
    T.bot = T.rows - 1;
    T.autowrap = true;
    T.cursor_visible = true;
    T.sx = T.sy = 0;
    T.spen = T.pen;
    T.pstate = P_GROUND;
    memset(&T.dec, 0, sizeof(T.dec));
}

static void reply(const char *s)
{
    if (T.reply != NULL)
        T.reply(T.reply_user, s, strlen(s));
}

static int param(int i, int def)
{
    if (i >= T.nparams || T.params[i] <= 0)
        return def;
    return T.params[i];
}

static void sgr(void)
{
    if (T.nparams == 0)
    {
        T.pen.fg = T.pen.bg = COLOR_DEFAULT;
        T.pen.attr = 0;
        return;
    }
    for (int i = 0; i < T.nparams; i++)
    {
        int p = T.params[i] < 0 ? 0 : T.params[i];

        if (p == 0)
        {
            T.pen.fg = T.pen.bg = COLOR_DEFAULT;
            T.pen.attr = 0;
        }
        else if (p == 1)
            T.pen.attr |= A_BOLD;
        else if (p == 2)
            T.pen.attr |= A_DIM;
        else if (p == 3)
            T.pen.attr |= A_ITALIC;
        else if (p == 4)
            T.pen.attr |= A_UNDER;
        else if (p == 5 || p == 6)
            T.pen.attr |= A_BLINK;
        else if (p == 7)
            T.pen.attr |= A_REVERSE;
        else if (p == 8)
            T.pen.attr |= A_HIDDEN;
        else if (p == 21 || p == 22)
            T.pen.attr &= (uint8_t) ~(A_BOLD | A_DIM);
        else if (p == 23)
            T.pen.attr &= (uint8_t) ~A_ITALIC;
        else if (p == 24)
            T.pen.attr &= (uint8_t) ~A_UNDER;
        else if (p == 25)
            T.pen.attr &= (uint8_t) ~A_BLINK;
        else if (p == 27)
            T.pen.attr &= (uint8_t) ~A_REVERSE;
        else if (p == 28)
            T.pen.attr &= (uint8_t) ~A_HIDDEN;
        else if (p >= 30 && p <= 37)
            T.pen.fg = (uint16_t) (p - 30);
        else if (p == 39)
            T.pen.fg = COLOR_DEFAULT;
        else if (p >= 40 && p <= 47)
            T.pen.bg = (uint16_t) (p - 40);
        else if (p == 49)
            T.pen.bg = COLOR_DEFAULT;
        else if (p >= 90 && p <= 97)
            T.pen.fg = (uint16_t) (p - 90 + 8);
        else if (p >= 100 && p <= 107)
            T.pen.bg = (uint16_t) (p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 2 < T.nparams && T.params[i + 1] == 5)
        {
            int c = T.params[i + 2];

            if (c >= 0 && c <= 255)
            {
                if (p == 38)
                    T.pen.fg = (uint16_t) c;
                else
                    T.pen.bg = (uint16_t) c;
            }
            i += 2;
        }
        else if ((p == 38 || p == 48) && i + 4 < T.nparams && T.params[i + 1] == 2)
        {
            /* Truecolour, to the nearest of the 6x6x6 cube. */
            int r = T.params[i + 2], g = T.params[i + 3], b = T.params[i + 4];
            int c = 16 + 36 * ((r < 0 ? 0 : r > 255 ? 255 : r) * 5 / 255) +
                    6 * ((g < 0 ? 0 : g > 255 ? 255 : g) * 5 / 255) + ((b < 0 ? 0 : b > 255 ? 255 : b) * 5 / 255);

            if (p == 38)
                T.pen.fg = (uint16_t) c;
            else
                T.pen.bg = (uint16_t) c;
            i += 4;
        }
    }
}

static void clamp_cursor(void)
{
    if (T.cx < 0)
        T.cx = 0;
    if (T.cx >= T.cols)
        T.cx = T.cols - 1;
    if (T.cy < 0)
        T.cy = 0;
    if (T.cy >= T.rows)
        T.cy = T.rows - 1;
}

static void csi_dispatch(char final)
{
    int n = param(0, 1);

    T.wrap_pending = false;
    if (T.priv == '?')
    {
        /* DEC private modes: autowrap and cursor visibility are the ones a
         * BBS uses. Everything else - mouse, alternate screen, keypad -
         * would change the real terminal, and is not ours to give. */
        if (final == 'h' || final == 'l')
            for (int i = 0; i < T.nparams; i++)
            {
                if (T.params[i] == 7)
                    T.autowrap = (final == 'h');
                else if (T.params[i] == 25)
                    T.cursor_visible = (final == 'h');
            }
        return;
    }
    if (T.priv != 0 || T.inter != 0)
        return;

    switch (final)
    {
    case 'A':
    {
        /* Up and down stop at the scrolling margins from inside them. */
        int lim = (T.cy >= T.top) ? T.top : 0;

        T.cy -= n;
        if (T.cy < lim)
            T.cy = lim;
        break;
    }
    case 'B':
    {
        int lim = (T.cy <= T.bot) ? T.bot : T.rows - 1;

        T.cy += n;
        if (T.cy > lim)
            T.cy = lim;
        break;
    }
    case 'C':
        T.cx += n;
        break;
    case 'D':
        T.cx -= n;
        break;
    case 'E':
        T.cy += n;
        T.cx = 0;
        break;
    case 'F':
        T.cy -= n;
        T.cx = 0;
        break;
    case 'G':
    case '`':
        T.cx = n - 1;
        break;
    case 'd':
        T.cy = n - 1;
        break;
    case 'H':
    case 'f':
        T.cy = param(0, 1) - 1;
        T.cx = param(1, 1) - 1;
        break;
    case 'J':
        switch (T.nparams ? T.params[0] : 0)
        {
        case 0:
            clamp_cursor();
            erase(T.cx, T.cy, T.cols - 1, T.rows - 1);
            break;
        case 1:
            clamp_cursor();
            erase(0, 0, T.cx, T.cy);
            break;
        default:
            /* ANSI.SYS homes the cursor too, and BBS screens are drawn
             * assuming it. */
            clear_screen();
            break;
        }
        break;
    case 'K':
        clamp_cursor();
        switch (T.nparams ? T.params[0] : 0)
        {
        case 0:
            erase(T.cx, T.cy, T.cols - 1, T.cy);
            break;
        case 1:
            erase(0, T.cy, T.cx, T.cy);
            break;
        default:
            erase(0, T.cy, T.cols - 1, T.cy);
            break;
        }
        break;
    case 'L':
        if (T.cy >= T.top && T.cy <= T.bot)
            scroll_down(T.cy, T.bot, n);
        T.cx = 0;
        break;
    case 'M':
        if (T.cy >= T.top && T.cy <= T.bot)
            scroll_up(T.cy, T.bot, n);
        T.cx = 0;
        break;
    case '@':
    case 'P':
    case 'X':
    {
        cell_t *row;
        int avail;

        clamp_cursor();
        row = cell_at(0, T.cy);
        avail = T.cols - T.cx;
        if (n > avail)
            n = avail;
        if (final == '@')
            memmove(&row[T.cx + n], &row[T.cx], sizeof(cell_t) * (size_t) (avail - n));
        else if (final == 'P')
            memmove(&row[T.cx], &row[T.cx + n], sizeof(cell_t) * (size_t) (avail - n));
        if (final == '@' || final == 'X')
            for (int x = T.cx; x < T.cx + n; x++)
                row[x] = blank_cell(&T.pen);
        else
            for (int x = T.cols - n; x < T.cols; x++)
                row[x] = blank_cell(&T.pen);
        break;
    }
    case 'S':
        scroll_up(T.top, T.bot, n);
        break;
    case 'T':
        scroll_down(T.top, T.bot, n);
        break;
    case 'm':
        sgr();
        break;
    case 'r':
    {
        int t = param(0, 1) - 1;
        int b = param(1, T.rows) - 1;

        if (b >= T.rows)
            b = T.rows - 1;
        if (t < b)
        {
            T.top = t;
            T.bot = b;
            T.cx = T.cy = 0;
        }
        break;
    }
    case 's':
        T.sx = T.cx;
        T.sy = T.cy;
        T.spen = T.pen;
        break;
    case 'u':
        T.cx = T.sx;
        T.cy = T.sy;
        T.pen = T.spen;
        break;
    case 'n':
        /* BBSes ask where the cursor is to find out whether the terminal
         * does ANSI at all; answer for the screen they are drawing on. */
        if (T.nparams > 0 && T.params[0] == 6)
        {
            char buf[32];

            clamp_cursor();
            snprintf(buf, sizeof(buf), "\033[%d;%dR", T.cy + 1, T.cx + 1);
            reply(buf);
        }
        else if (T.nparams > 0 && T.params[0] == 5)
        {
            reply("\033[0n");
        }
        break;
    case 'c':
        if (T.nparams == 0 || T.params[0] == 0)
            reply("\033[?1;0c");
        break;
    default:
        break;
    }
    clamp_cursor();
}

static void control(unsigned char b)
{
    switch (b)
    {
    case '\a':
        T.bell = true;
        break;
    case '\b':
        if (T.cx > 0)
            T.cx--;
        T.wrap_pending = false;
        break;
    case '\t':
        T.cx = (T.cx / 8 + 1) * 8;
        if (T.cx >= T.cols)
            T.cx = T.cols - 1;
        break;
    case '\n':
    case '\v':
        linefeed();
        T.wrap_pending = false;
        break;
    case '\f':
        /* A form feed clears the screen, as the BBS terminals did it. */
        clear_screen();
        break;
    case '\r':
        T.cx = 0;
        T.wrap_pending = false;
        break;
    default:
        break; /* NUL, SO, SI and the rest: nothing */
    }
}

static void feed(unsigned char b)
{
    switch (T.pstate)
    {
    case P_GROUND:
        if (b == 0x1B)
        {
            T.pstate = P_ESC;
            return;
        }
        if (b < 0x20 || b == 0x7F)
        {
            if (b != 0x7F)
                control(b);
            return;
        }
        {
            int32_t cp = decode(&T.dec, g_charset, b);

            if (cp >= 0x20)
                put_char((uint32_t) cp);
        }
        return;

    case P_ESC:
        T.pstate = P_GROUND;
        switch (b)
        {
        case '[':
            T.pstate = P_CSI;
            T.nparams = 0;
            T.param_started = false;
            T.priv = 0;
            T.inter = 0;
            memset(T.params, 0, sizeof(T.params));
            break;
        case ']':
        case 'P':
        case 'X':
        case '^':
        case '_':
            T.pstate = P_STRING;
            T.string_len = 0;
            break;
        case '(':
        case ')':
        case '*':
        case '+':
        case '#':
        case '%':
            T.pstate = P_ESC_SKIP1;
            break;
        case '7':
            T.sx = T.cx;
            T.sy = T.cy;
            T.spen = T.pen;
            break;
        case '8':
            T.cx = T.sx;
            T.cy = T.sy;
            T.pen = T.spen;
            T.wrap_pending = false;
            clamp_cursor(); /* saved before the window last shrank, perhaps */
            break;
        case 'D':
            linefeed();
            break;
        case 'E':
            T.cx = 0;
            linefeed();
            break;
        case 'M':
            reverse_index();
            break;
        case 'c':
            reset_state();
            clear_screen();
            break;
        case 0x1B:
            T.pstate = P_ESC;
            break;
        default:
            break;
        }
        return;

    case P_ESC_SKIP1:
        T.pstate = P_GROUND;
        return;

    case P_CSI:
        if (b >= '0' && b <= '9')
        {
            if (T.nparams == 0)
                T.nparams = 1;
            if (T.params[T.nparams - 1] < 100000)
                T.params[T.nparams - 1] = T.params[T.nparams - 1] * 10 + (b - '0');
            T.param_started = true;
        }
        else if (b == ';' || b == ':')
        {
            if (T.nparams == 0)
                T.nparams = 1;
            if (T.nparams < MAX_PARAMS)
                T.params[T.nparams++] = 0;
        }
        else if (b >= '<' && b <= '?')
        {
            T.priv = (char) b;
        }
        else if (b >= 0x20 && b <= 0x2F)
        {
            T.inter = (char) b;
        }
        else if (b >= 0x40 && b <= 0x7E)
        {
            T.pstate = P_GROUND;
            csi_dispatch((char) b);
        }
        else if (b == 0x1B)
        {
            T.pstate = P_ESC;
        }
        else if (b < 0x20)
        {
            control(b); /* VT100s act on controls in the middle of a sequence */
        }
        else
        {
            T.pstate = P_GROUND;
        }
        return;

    case P_STRING:
        if (b == 0x07 || ++T.string_len > 4096)
            T.pstate = P_GROUND;
        else if (b == 0x1B)
            T.pstate = P_STRING_ESC;
        return;

    case P_STRING_ESC:
        T.pstate = (b == '\\') ? P_GROUND : P_STRING;
        return;
    }
}

/* ------------------------------------------------------------ status line */

static void human_bytes(char *out, size_t len, uint64_t n)
{
    if (n < 10000)
        snprintf(out, len, "%" PRIu64, n);
    else if (n < 10000000)
        snprintf(out, len, "%.1fK", (double) n / 1024.0);
    else
        snprintf(out, len, "%.1fM", (double) n / (1024.0 * 1024.0));
}

typedef struct
{
    const char *text;
    int priority;               /* the higher, the later it is dropped for lack of room */
    bool right;                 /* goes at the right-hand end */
} segment_t;

static void draw_status(bool force)
{
    char line[512];
    char head[48];
    char state[200];
    char rate[64];
    char snr[24];
    char traffic[48];
    char hint[32];
    segment_t seg[8];
    int nseg = 0;
    int64_t now = dm_now_ms();
    int64_t t;
    bool online;
    bool notice;
    size_t width = (size_t) T.W - 1;
    size_t o;

    pthread_mutex_lock(&T.lock);
    online = T.connected_ms != 0 && T.ended_ms == 0;
    t = ((T.ended_ms ? T.ended_ms : now) - (T.connected_ms ? T.connected_ms : T.since_ms)) / 1000;
    snprintf(head, sizeof(head), " %-11s %02d:%02d:%02d", T.label, (int) (t / 3600), (int) (t / 60 % 60),
             (int) (t % 60));
    notice = T.notice_until > now;
    snprintf(state, sizeof(state), "%s", notice ? T.notice : T.detail);
    snprintf(rate, sizeof(rate), "%s", T.connected_ms ? T.rate : "");
    snprintf(snr, sizeof(snr), "%s", online ? T.snr : "");
    snprintf(traffic, sizeof(traffic), "%s", T.connected_ms ? T.traffic : "");
    snprintf(hint, sizeof(hint), "%s",
             T.ended_ms ? "" : (T.command_mode ? "ATO returns online" : (online ? "+++ for commands" : "ctrl-c quits")));
    pthread_mutex_unlock(&T.lock);

    /* What matters most survives a narrow window: what is happening, then
     * the rate, then how to get out of it. */
    seg[nseg++] = (segment_t) { head, 100, false };
    if (rate[0] != '\0')
        seg[nseg++] = (segment_t) { rate, 80, false };
    /* Once the call is over, why it ended matters more than how fast it was. */
    if (state[0] != '\0')
        seg[nseg++] = (segment_t) { state, (notice || !online) ? 90 : 60, false };
    if (traffic[0] != '\0')
        seg[nseg++] = (segment_t) { traffic, 50, false };
    if (snr[0] != '\0')
        seg[nseg++] = (segment_t) { snr, 40, false };
    seg[nseg++] = (segment_t) { charset_name(g_charset), 30, true };
    if (hint[0] != '\0')
        seg[nseg++] = (segment_t) { hint, 70, true };

    for (;;)
    {
        size_t need = 0;
        int lowest = -1;

        for (int i = 0; i < nseg; i++)
            if (seg[i].text != NULL)
            {
                need += strlen(seg[i].text) + 3;
                if (i > 0 && (lowest < 0 || seg[i].priority < seg[lowest].priority))
                    lowest = i;
            }
        if (need <= width || lowest < 0)
            break;
        seg[lowest].text = NULL;
    }

    o = 0;
    for (int i = 0; i < nseg && o < sizeof(line); i++)
        if (seg[i].text != NULL && !seg[i].right)
            o += (size_t) snprintf(line + o, sizeof(line) - o, i == 0 ? "%s " : "| %s ", seg[i].text);
    {
        char right[96] = "";
        size_t r = 0;

        for (int i = 0; i < nseg; i++)
            if (seg[i].text != NULL && seg[i].right && r < sizeof(right))
                r += (size_t) snprintf(right + r, sizeof(right) - r, r == 0 ? "%s " : "| %s ", seg[i].text);
        while (o + r < width && o < sizeof(line) - 1)
            line[o++] = ' ';
        if (o < sizeof(line))
            o += (size_t) snprintf(line + o, sizeof(line) - o, "%s", right);
    }
    if (o > width)
        o = width;
    while (o < width && o < sizeof(line) - 1)
        line[o++] = ' ';
    line[o] = '\0';

    if (!force && strcmp(line, T.drawn_status) == 0)
        return;
    snprintf(T.drawn_status, sizeof(T.drawn_status), "%s", line);

    /* Reverse video, the bottom row, never the last column - writing there
     * can scroll some terminals. */
    {
        pen_t bar = { COLOR_DEFAULT, COLOR_DEFAULT, A_REVERSE };

        out_move(0, T.H - 1);
        out_pen(&bar);
        out_str(line);
        T.px = -1;
    }
}

/* ------------------------------------------------------------ the log */

/* Lines the log would have put on the screen: kept off it while it is ours,
 * warnings shown on the status line for a few seconds, errors kept to be
 * printed when it is handed back. Any thread. */
static void log_tap(dm_log_level_t level, const char *component, const char *msg)
{
    (void) component;
    if (level > DM_LOG_WARN)
        return;
    pthread_mutex_lock(&T.lock);
    snprintf(T.notice, sizeof(T.notice), "%s", msg);
    T.notice_until = dm_now_ms() + (level == DM_LOG_ERROR ? 10000 : 6000);
    if (level == DM_LOG_ERROR)
    {
        if (T.nerrors == (int) (sizeof(T.errors) / sizeof(T.errors[0])))
        {
            memmove(T.errors[0], T.errors[1], sizeof(T.errors) - sizeof(T.errors[0]));
            T.nerrors--;
        }
        snprintf(T.errors[T.nerrors++], sizeof(T.errors[0]), "%s", msg);
    }
    pthread_mutex_unlock(&T.lock);
}

/* ------------------------------------------------------------ public */

static bool layout(void)
{
    struct winsize ws;
    int W = 80, H = 24;
    cell_t *cells;
    cell_t *shown;
    int rows;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    {
        W = ws.ws_col;
        H = ws.ws_row;
    }
    if (W < 20 || H < 3)
        return false;
    rows = H - 1;
    cells = calloc((size_t) (rows * W), sizeof(cell_t));
    shown = calloc((size_t) (rows * W), sizeof(cell_t));
    if (cells == NULL || shown == NULL)
    {
        free(cells);
        free(shown);
        return false;
    }
    for (int i = 0; i < rows * W; i++)
        cells[i] = blank_cell(NULL);

    /* What was on the screen before carries over, as much as fits, keeping
     * the cursor's line in view. */
    if (T.cells != NULL)
    {
        int off = (T.cy >= rows) ? T.cy - rows + 1 : 0;
        int ncols = (T.cols < W) ? T.cols : W;

        for (int y = 0; y < rows && y + off < T.rows; y++)
            for (int x = 0; x < ncols; x++)
                cells[y * W + x] = T.cells[(y + off) * T.cols + x];
        T.cy -= off;
        free(T.cells);
        free(T.shown);
    }
    T.cells = cells;
    T.shown = shown;
    T.W = W;
    T.H = H;
    T.rows = rows;
    T.cols = W;
    T.top = 0;
    T.bot = rows - 1;
    clamp_cursor();
    if (T.sx >= W)
        T.sx = W - 1;
    if (T.sy >= rows)
        T.sy = rows - 1;
    T.wrap_pending = false;
    invalidate_shown();

    /* Clear the lot, and fence the status line off from scrolling. */
    out_str("\033[0m\033[2J");
    out_fmt("\033[1;%dr", rows);
    T.ppen_known = false;
    T.px = T.py = -1;
    T.drawn_status[0] = '\0';
    return true;
}

bool dm_term_start(const dm_config_t *cfg)
{
    struct sigaction sa;
    const char *codeset;

    g_charset = charset_parse(cfg->charset);
    setlocale(LC_CTYPE, "");
    codeset = nl_langinfo(CODESET);
    g_out_utf8 = (codeset != NULL && (strcasecmp(codeset, "UTF-8") == 0 || strcasecmp(codeset, "UTF8") == 0));

    if (T.active || !cfg->tui || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return false;
    T.cx = T.cy = 0;
    if (!layout())
        return false;
    reset_state();
    T.pcursor_visible = true;
    T.active = true;
    T.stopped = false;
    T.since_ms = dm_now_ms();
    T.ended_ms = 0;
    T.connected_ms = 0;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, &T.old_winch);

    /* Keystrokes typed while dialling must not land on the screen; ctrl-c
     * still quits. */
    dm_tty_quiet();

    /* From here the log stays off the screen: see log_tap(). */
    dm_log_set_tap(log_tap);
    dm_log_set_terminal_floor((int) DM_LOG_ERROR - 1);

    atexit(dm_term_stop);
    dm_term_state("STARTING", "datamodem %s", DATAMODEM_VERSION);
    render();
    return true;
}

bool dm_term_active(void)
{
    return T.active;
}

void dm_term_set_reply(void (*fn)(void *user, const void *data, size_t len), void *user)
{
    T.reply = fn;
    T.reply_user = user;
}

static void check_resize(void)
{
    if (!g_winch)
        return;
    g_winch = 0;
    layout();
}

void dm_term_remote(const void *data, size_t len)
{
    const unsigned char *p = data;

    if (!T.active)
        return;
    check_resize();
    for (size_t i = 0; i < len; i++)
        feed(p[i]);
    render();
}

void dm_term_local(const void *text, size_t len)
{
    const unsigned char *p = text;
    pen_t saved = T.pen;

    if (!T.active)
        return;
    check_resize();
    /* Our own words, in the terminal's plain colours, and never mistaken
     * for the middle of something the far end was saying. */
    T.pen.fg = T.pen.bg = COLOR_DEFAULT;
    T.pen.attr = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char b = p[i];

        if (b < 0x20 || b == 0x7F)
        {
            if (b == '\r' || b == '\n' || b == '\b' || b == '\a')
                control(b);
            continue;
        }
        {
            int32_t cp = decode(&T.local_dec, CS_UTF8, b);

            if (cp >= 0x20)
                put_char((uint32_t) cp);
        }
    }
    T.pen = saved;
    render();
}

void dm_term_state(const char *label, const char *detail_fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&T.lock);
    snprintf(T.label, sizeof(T.label), "%s", label);
    va_start(ap, detail_fmt);
    vsnprintf(T.detail, sizeof(T.detail), detail_fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&T.lock);
}

void dm_term_link(const dm_modem_status_t *st, int64_t connected_ms, bool command_mode)
{
    char rx[16];
    char tx[16];
    char rate[48];

    human_bytes(rx, sizeof(rx), st->bytes_rx);
    human_bytes(tx, sizeof(tx), st->bytes_tx);
    if (strcmp(st->protocol, "async") == 0)
        snprintf(rate, sizeof(rate), "%d", st->bit_rate);
    else
        snprintf(rate, sizeof(rate), "%d %s", st->bit_rate, st->protocol);
    pthread_mutex_lock(&T.lock);
    T.connected_ms = connected_ms;
    T.command_mode = command_mode;
    snprintf(T.rate, sizeof(T.rate), "%s", rate);
    if (st->train_stage != NULL && st->snr_db > 0.0f)
        snprintf(T.snr, sizeof(T.snr), "SNR %.0f dB", (double) st->snr_db);
    else
        T.snr[0] = '\0';
    snprintf(T.traffic, sizeof(T.traffic), "RX %s TX %s", rx, tx);
    pthread_mutex_unlock(&T.lock);
}

void dm_term_end(const char *why)
{
    pthread_mutex_lock(&T.lock);
    if (T.ended_ms == 0)
        T.ended_ms = dm_now_ms();
    snprintf(T.label, sizeof(T.label), "%s", "OFFLINE");
    snprintf(T.detail, sizeof(T.detail), "%s", why ? why : "");
    T.notice_until = 0;
    pthread_mutex_unlock(&T.lock);
}

void dm_term_fresh_line(void)
{
    if (!T.active)
        return;
    if (T.cx != 0 || T.wrap_pending)
    {
        T.cx = 0;
        T.wrap_pending = false;
        linefeed();
    }
}

void dm_term_tick(void)
{
    if (!T.active)
        return;
    if (g_winch)
    {
        check_resize();
        render();
        return;
    }
    draw_status(false);
    out_move(T.cx, T.cy);
    out_flush();
}

void dm_term_stop(void)
{
    int nerrors;
    char errors[6][240];

    if (!T.active || T.stopped)
        return;
    T.stopped = true;
    dm_tty_restore();
    pthread_mutex_lock(&T.lock);
    if (T.ended_ms == 0)
        T.ended_ms = dm_now_ms();
    pthread_mutex_unlock(&T.lock);
    render();
    draw_status(true);

    /* Hand the screen back: margins off (which homes the cursor), to the
     * status line, and a new line below it for the shell. */
    out_str("\033[0m\033[?25h\033[r");
    T.px = T.py = -1;
    out_move(0, T.H - 1);
    out_str("\r\n");
    out_flush();

    sigaction(SIGWINCH, &T.old_winch, NULL);
    T.active = false;
    dm_log_set_tap(NULL);
    /* Anything said from here on is said below the status line, like any
     * other program's output - but only what matters. */
    dm_log_set_terminal_floor(DM_LOG_WARN);

    pthread_mutex_lock(&T.lock);
    nerrors = T.nerrors;
    memcpy(errors, T.errors, sizeof(errors));
    T.nerrors = 0;
    pthread_mutex_unlock(&T.lock);
    for (int i = 0; i < nerrors; i++)
        fprintf(stderr, "datamodem: %s\n", errors[i]);
    fflush(stderr);

    free(T.cells);
    free(T.shown);
    T.cells = T.shown = NULL;
}

/* ------------------------------------------------------------ sanitizer */

static struct
{
    int state;          /* 0 text, 1 ESC, 2 CSI, 3 string, 4 rest of a CSI */
    char seq[64];
    size_t seqlen;
    decoder_t dec;
    bool coloured;
    /* Where the far end thinks its cursor is, on a screen the size of the
     * terminal, for answering it when it asks - which is how most BBSes
     * decide whether the terminal does ANSI, and some how big it is. Most
     * of what moves it never reaches the terminal, so it is the screen the
     * far end believes it is drawing, not where the text is landing. cx may
     * be cols: the last column written, the wrap still to come. */
    bool placed;
    int cx, cy, sx, sy;
    int cols, rows;
} S;

static void sanitize_size(void)
{
    struct winsize ws;

    S.cols = 80;
    S.rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    {
        S.cols = ws.ws_col;
        S.rows = ws.ws_row;
    }
    if (!S.placed)
    {
        /* Line output goes on at the bottom of a terminal that scrolls. */
        S.cy = S.rows - 1;
        S.placed = true;
    }
    if (S.cx > S.cols)
        S.cx = S.cols;
    if (S.cy >= S.rows)
        S.cy = S.rows - 1;
}

static int sanitize_param(int i, int def)
{
    int n = 0, v = 0;

    for (size_t k = 0; k <= S.seqlen; k++)
    {
        if (k == S.seqlen || S.seq[k] == ';')
        {
            if (n == i)
                return v > 0 ? v : def;
            n++;
            v = 0;
        }
        else if (v < 10000)
        {
            v = v * 10 + (S.seq[k] - '0');
        }
    }
    return def;
}

/* A CSI sequence's effect on where the far end thinks the cursor is, and
 * the answers to its queries - the same ones the emulator gives. */
static void sanitize_csi(unsigned char final)
{
    int n = sanitize_param(0, 1);

    switch (final)
    {
    case 'A':
        S.cy -= n;
        break;
    case 'B':
        S.cy += n;
        break;
    case 'C':
        S.cx += n;
        break;
    case 'D':
        S.cx = (S.cx < S.cols ? S.cx : S.cols - 1) - n;
        break;
    case 'G':
        S.cx = n - 1;
        break;
    case 'd':
        S.cy = n - 1;
        break;
    case 'H':
    case 'f':
        S.cy = n - 1;
        S.cx = sanitize_param(1, 1) - 1;
        break;
    case 'J':
        if (sanitize_param(0, 0) == 2) /* ANSI.SYS homes the cursor */
            S.cx = S.cy = 0;
        break;
    case 's':
        S.sx = S.cx;
        S.sy = S.cy;
        break;
    case 'u':
        S.cx = S.sx;
        S.cy = S.sy;
        break;
    case 'n':
        if (sanitize_param(0, 0) == 6)
        {
            char buf[32];

            snprintf(buf, sizeof(buf), "\033[%d;%dR", S.cy + 1, (S.cx < S.cols ? S.cx : S.cols - 1) + 1);
            reply(buf);
        }
        else if (sanitize_param(0, 0) == 5)
        {
            reply("\033[0n");
        }
        break;
    case 'c':
        if (sanitize_param(0, 0) == 0)
            reply("\033[?1;0c");
        break;
    default:
        break;
    }
    if (S.cx < 0)
        S.cx = 0;
    if (S.cx >= S.cols && strchr("ABCDGHdfJu", final) != NULL) /* moving ends a pending wrap */
        S.cx = S.cols - 1;
    if (S.cy < 0)
        S.cy = 0;
    if (S.cy >= S.rows)
        S.cy = S.rows - 1;
}

size_t dm_term_sanitize(const unsigned char *in, size_t len, char *out)
{
    size_t o = 0;

    sanitize_size();
    for (size_t i = 0; i < len; i++)
    {
        unsigned char b = in[i];

        switch (S.state)
        {
        case 0:
            if (b == 0x1B)
            {
                S.state = 1;
            }
            else if (b == '\r' || b == '\n' || b == '\b' || b == '\t' || b == '\a')
            {
                out[o++] = (char) b;
                if (b == '\r')
                    S.cx = 0;
                else if (b == '\n' && S.cy < S.rows - 1)
                    S.cy++;
                else if (b == '\b' && S.cx > 0)
                    S.cx = (S.cx < S.cols ? S.cx : S.cols - 1) - 1;
                else if (b == '\t')
                    S.cx = S.cx / 8 * 8 + 8 < S.cols ? S.cx / 8 * 8 + 8 : S.cols - 1;
            }
            else if (b >= 0x20 && b != 0x7F)
            {
                int32_t cp = decode(&S.dec, g_charset, b);

                if (cp >= 0x20)
                {
                    o += encode((uint32_t) cp, out + o);
                    if (S.cx >= S.cols)
                    {
                        S.cx = 0;
                        if (S.cy < S.rows - 1)
                            S.cy++;
                    }
                    S.cx++;
                }
            }
            break;
        case 1:
            if (b == '[')
            {
                S.state = 2;
                S.seqlen = 0;
            }
            else if (b == ']' || b == 'P' || b == 'X' || b == '^' || b == '_')
            {
                S.state = 3;
                S.seqlen = 0;
            }
            else
            {
                if (b == '7')
                {
                    S.sx = S.cx;
                    S.sy = S.cy;
                }
                else if (b == '8')
                {
                    S.cx = S.sx;
                    S.cy = S.sy;
                }
                S.state = 0;
            }
            break;
        case 2:
            /* Digits and separators, then one of the finals that act on
             * nothing but the current line: colour (m), cursor forward and
             * back (C, D), to a column (G), erase in line (K). BBSes indent
             * and space their screens with C and G rather than spaces, so
             * without them a banner collapses against the left margin. Up,
             * down, to a row, clearing the screen and every private or
             * intermediate form stay out: they reach beyond the line, into
             * the shell's own output or the terminal's state. Queries are
             * answered here instead, as the emulator answers them. */
            if ((b >= '0' && b <= '9') || b == ';')
            {
                if (S.seqlen < sizeof(S.seq) - 1)
                    S.seq[S.seqlen++] = (char) b;
                else
                    S.state = 0;
            }
            else
            {
                if (b == 'm' || b == 'C' || b == 'D' || b == 'G' || b == 'K')
                {
                    out[o++] = 0x1B;
                    out[o++] = '[';
                    memcpy(out + o, S.seq, S.seqlen);
                    o += S.seqlen;
                    out[o++] = (char) b;
                    if (b == 'm')
                        S.coloured = true;
                }
                if (b >= 0x40 && b <= 0x7E)
                    sanitize_csi(b);
                S.state = (b >= 0x20 && b < 0x40) ? 4 : 0; /* 4: swallow the rest */
            }
            break;
        case 4:
            if (b >= 0x40 && b <= 0x7E)
                S.state = 0;
            break;
        default:
            if (b == 0x07 || b == '\\' || ++S.seqlen > 4096)
                S.state = 0;
            break;
        }
    }
    return o;
}

void dm_term_sanitize_end(void)
{
    if (S.coloured)
    {
        dm_tty_write_raw("\033[0m", 4);
        S.coloured = false;
    }
}
