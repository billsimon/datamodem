#include "datamodem/config.h"
#include "datamodem/modem.h"
#include "datamodem/util.h"
#include "datamodem/version.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    OPT_STR,
    OPT_INT,
    OPT_BOOL,
    OPT_KEY,       /* a character: 'X', 0x1b, ^], or none */
    OPT_LOGLEVEL
} opt_kind_t;

typedef struct
{
    const char *name;   /* long flag name, also the config-file and env key */
    opt_kind_t kind;
    size_t offset;      /* into dm_config_t */
    size_t size;        /* for OPT_STR */
    long min;
    long max;
    const char *help;
} opt_def_t;

#define STR_OPT(name, field, help) \
    {name, OPT_STR, offsetof(dm_config_t, field), sizeof(((dm_config_t *) 0)->field), 0, 0, help}
#define INT_OPT(name, field, lo, hi, help) \
    {name, OPT_INT, offsetof(dm_config_t, field), 0, lo, hi, help}
#define BOOL_OPT(name, field, help) \
    {name, OPT_BOOL, offsetof(dm_config_t, field), 0, 0, 0, help}
#define KEY_OPT(name, field, help) \
    {name, OPT_KEY, offsetof(dm_config_t, field), 0, 0, 0, help}

static const opt_def_t OPTS[] = {
    /* SIP account */
    STR_OPT("server", server, "SIP registrar/domain, e.g. sip.example.com[:5060]"),
    STR_OPT("username", username, "SIP user part"),
    STR_OPT("auth-user", auth_user, "digest auth username (defaults to --username)"),
    STR_OPT("password", password, "digest password (prefer DATAMODEM_PASSWORD)"),
    STR_OPT("realm", realm, "digest realm (default *)"),
    STR_OPT("from", from_uri, "override the From/contact URI"),
    STR_OPT("proxy", proxy, "outbound proxy URI, e.g. sip:edge.example.com;lr"),
    STR_OPT("caller-id", caller_id,
            "asserted caller ID: sets P-Asserted-Identity on the INVITE (and the From display name)"),
    BOOL_OPT("register", do_register,
             "maintain a SIP registration (default: off when dialling, on when answering)"),
    INT_OPT("reg-timeout", reg_timeout_s, 1, 600, "seconds to wait for REGISTER to succeed"),
    INT_OPT("reg-expires", reg_expires_s, 30, 86400, "registration expiry requested"),

    /* Transport */
    STR_OPT("transport", transport, "udp | tcp | tls (default udp)"),
    STR_OPT("bind-addr", bind_addr, "local address to bind SIP to"),
    INT_OPT("local-port", local_port, 0, 65535, "local SIP port (0 = ephemeral)"),
    STR_OPT("public-addr", public_addr, "advertised address when behind NAT"),
    STR_OPT("stun", stun_server, "STUN server for NAT discovery"),
    STR_OPT("nameserver", nameserver, "DNS server for SRV/NAPTR resolution"),
    INT_OPT("rtp-port", rtp_port, 0, 65535, "base RTP port"),
    INT_OPT("rtp-port-range", rtp_port_range, 0, 60000,
            "ports above --rtp-port that media may use (default 100); open exactly this range"),

    /* Media */
    STR_OPT("codec", codec, "pcmu | pcma (default pcmu)"),
    INT_OPT("jitter-buffer-ms", jitter_buffer_ms, 0, 500,
            "fixed jitter buffer size (default 150); smaller starves the modem on a real path"),
    BOOL_OPT("user-phone", user_phone, "append ;user=phone to the request URI"),

    /* Modem */
    STR_OPT("modulation", modulation,
            "v32bis | v32 | v22bis | v22 | v23 | v21 | bell103 (default v21; v32bis is 14400 bps, "
            "v32 9600, v22bis 2400)"),
    INT_OPT("bit-rate", bit_rate, 0, 14400,
            "rate ceiling: 14400, 12000, 9600, 7200 or 4800 for v32bis, 9600 or 4800 for v32, 2400 "
            "or 1200 for v22bis (default 0, the most the modulation can do)"),
    STR_OPT("guard-tone", guard_tone,
            "none | 550 | 1800; the answering modem's guard tone (default none)"),
    INT_OPT("data-bits", data_bits, 5, 8, "character length (default 8)"),
    STR_OPT("parity", parity, "none | even | odd (default none)"),
    INT_OPT("stop-bits", stop_bits, 1, 2, "stop bits (default 1)"),
    BOOL_OPT("v14", v14, "V.14 rate adaption when framing received characters (default on)"),
    INT_OPT("tx-power", tx_power, -30, 0, "transmit level in dBm0 (default -13)"),
    INT_OPT("answer-tone-ms", answer_tone_ms, 0, 10000,
            "length of the 2100 Hz answer tone when answering (default 3300, 0 = none)"),
    INT_OPT("answer-wait", answer_wait_s, 0, 120,
            "seconds the caller lets the answering end announce itself before training "
            "(default 5, 0 = train immediately)"),
    INT_OPT("answer-tail-ms", answer_tail_ms, 0, 10000,
            "if the answer tone is heard, extend the wait to this long after hearing it "
            "(default 3300; V.25 allows the tone up to 4s)"),
    BOOL_OPT("calling-tone", calling_tone, "send the V.25 1300 Hz calling tone while waiting"),
    INT_OPT("train-timeout", train_timeout_s, 5, 600,
            "give up if the modems have not trained by then (default 45)"),
    INT_OPT("max-retrains", max_retrains, 0, 100,
            "give up on a link that keeps retraining (default 4, 0 = never give up)"),

    /* Error correction and compression */
    STR_OPT("v42", v42,
            "V.42 error correction: off (default) | detect (fall back to async) | require"),
    INT_OPT("v42-timeout", v42_timeout_s, 2, 120,
            "seconds to keep trying to establish V.42 before falling back (default 10)"),
    BOOL_OPT("v42bis", v42bis, "V.42bis compression; needs --v42"),
    INT_OPT("v42bis-dict", v42bis_dict, 512, 4096,
            "most V.42bis dictionary to offer, in codewords (default 2048); the far end may take less"),
    INT_OPT("v42bis-max-string", v42bis_max_string, 6, 250,
            "longest V.42bis string to offer (default 32); the far end may take less"),

    /* Session */
    KEY_OPT("escape-char", escape_char,
            "Hayes S2: the character typed three times to reach command mode (default +)"),
    INT_OPT("escape-guard-ms", escape_guard_ms, 100, 10000,
            "Hayes S12: silence needed either side of the escape sequence (default 1000)"),
    KEY_OPT("escape-key", escape_key,
            "an extra single-keystroke escape, e.g. ^] - off by default because a real modem has none"),
    BOOL_OPT("local-echo", local_echo, "echo what you type, for a far end that does not (ATE1)"),
    INT_OPT("connect-timeout", connect_timeout_s, 5, 600,
            "seconds to wait for the call to be answered (default 60)"),
    INT_OPT("answer-timeout", answer_timeout_s, 0, 86400,
            "seconds to wait for an inbound call (default 0 = forever)"),
    INT_OPT("idle-timeout", idle_timeout_s, 0, 86400,
            "hang up after this long with nothing received (default 0 = never)"),
    INT_OPT("media-timeout", media_timeout_s, 0, 600,
            "hang up after this many seconds with no inbound RTP (default 20, 0 disables)"),
    INT_OPT("max-call", max_call_s, 0, 86400, "hard ceiling on one call, seconds (default 0 = none)"),

    /* Logging */
    {"log-level", OPT_LOGLEVEL, offsetof(dm_config_t, log_level), 0, 0, 0,
     "error | warn | info | debug | trace (default info)"},
    BOOL_OPT("log-json", log_json, "emit one JSON object per line"),
    STR_OPT("log-file", log_file,
            "write the log here instead of stderr - what you want during a session"),
    INT_OPT("pjsip-log-level", pjsip_log_level, 0, 6, "override pjsip verbosity"),
    INT_OPT("spandsp-log-level", spandsp_log_level, 0, 10, "override spandsp verbosity"),
};

static const size_t N_OPTS = sizeof(OPTS) / sizeof(OPTS[0]);

void dm_config_defaults(dm_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->command = DM_CMD_NONE;
    snprintf(cfg->realm, sizeof(cfg->realm), "%s", "*");
    snprintf(cfg->transport, sizeof(cfg->transport), "%s", "udp");
    snprintf(cfg->codec, sizeof(cfg->codec), "%s", "pcmu");
    /* V.21 rather than the eight-times-faster V.22bis, because 300 bps FSK
     * will get through an audio path that QAM will not, and a default should
     * connect rather than be quick. --modulation v22bis when the line is
     * good. (V.22bis was previously excluded because spandsp's was broken;
     * it is not any more - see third_party/spandsp-v22bis.) */
    snprintf(cfg->modulation, sizeof(cfg->modulation), "%s", "v21");
    snprintf(cfg->guard_tone, sizeof(cfg->guard_tone), "%s", "none");
    snprintf(cfg->parity, sizeof(cfg->parity), "%s", "none");

    cfg->do_register = false;
    cfg->reg_timeout_s = 30;
    cfg->reg_expires_s = 300;
    cfg->rtp_port_range = 100;
    cfg->jitter_buffer_ms = 150;

    cfg->bit_rate = 0;
    cfg->data_bits = 8;
    cfg->stop_bits = 1;
    cfg->v14 = true;
    cfg->tx_power = -13;
    /* V.25 says 2.6 to 4 seconds of answer tone; 3.3 sits in the middle and
     * is what most equipment sends. */
    cfg->answer_tone_ms = 3300;
    /* Long enough to sit out a full-length V.25 answer tone (2.6-4.0s)
     * whether or not the detector notices it. */
    cfg->answer_wait_s = 5;
    cfg->answer_tail_ms = 3300;
    cfg->train_timeout_s = 45;
    cfg->max_retrains = 4;

    /* Off by default. V.42's detection phase puts ODP patterns on the line,
     * and spandsp keeps sending them forever against a far end that does not
     * answer - so turning this on blind would spray junk at a plain async
     * host for --v42-timeout seconds before falling back. */
    snprintf(cfg->v42, sizeof(cfg->v42), "%s", "off");
    cfg->v42_timeout_s = 10;
    /* What real modems shipped with, and three times better than the 512/6
     * that spandsp's own XID advertises. Since nothing is actually
     * negotiated, what matters is that both ends agree. */
    cfg->v42bis_dict = 2048;
    cfg->v42bis_max_string = 32;

    cfg->escape_char = '+';
    cfg->escape_guard_ms = 1000;
    cfg->escape_key = -1;
    cfg->connect_timeout_s = 60;
    cfg->answer_timeout_s = 0;
    cfg->idle_timeout_s = 0;
    cfg->media_timeout_s = 20;
    cfg->max_call_s = 0;

    cfg->log_level = DM_LOG_INFO;
    cfg->pjsip_log_level = -1;
    cfg->spandsp_log_level = -1;
}

static const opt_def_t *find_opt(const char *key)
{
    for (size_t i = 0; i < N_OPTS; i++)
    {
        if (strcmp(OPTS[i].name, key) == 0)
            return &OPTS[i];
    }
    return NULL;
}

static bool parse_bool(const char *v, bool *out)
{
    if (v == NULL || *v == '\0')
    {
        *out = true;
        return true;
    }
    if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "on") == 0)
    {
        *out = true;
        return true;
    }
    if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 ||
        strcasecmp(v, "off") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

bool dm_config_set(dm_config_t *cfg, const char *key, const char *value, char *err, size_t err_len)
{
    bool negate = false;
    const opt_def_t *def = find_opt(key);
    char *base = (char *) cfg;

    if (def == NULL && strncmp(key, "no-", 3) == 0)
    {
        def = find_opt(key + 3);
        if (def != NULL && def->kind == OPT_BOOL)
            negate = true;
        else
            def = NULL;
    }
    if (def == NULL)
    {
        snprintf(err, err_len, "unknown option '%s'", key);
        return false;
    }

    switch (def->kind)
    {
    case OPT_STR:
        if (value == NULL)
        {
            snprintf(err, err_len, "option '%s' needs a value", key);
            return false;
        }
        if (strlen(value) >= def->size)
        {
            snprintf(err, err_len, "value for '%s' is too long (max %zu chars)", key, def->size - 1);
            return false;
        }
        snprintf(base + def->offset, def->size, "%s", value);
        return true;

    case OPT_INT:
    {
        char *end = NULL;
        long n;
        if (value == NULL)
        {
            snprintf(err, err_len, "option '%s' needs a value", key);
            return false;
        }
        errno = 0;
        n = strtol(value, &end, 10);
        if (errno != 0 || end == value || *end != '\0')
        {
            snprintf(err, err_len, "value for '%s' is not a number: '%s'", key, value);
            return false;
        }
        if (n < def->min || n > def->max)
        {
            snprintf(err, err_len, "value for '%s' must be between %ld and %ld", key, def->min, def->max);
            return false;
        }
        *(int *) (base + def->offset) = (int) n;
        return true;
    }

    case OPT_BOOL:
    {
        bool b;
        if (!parse_bool(value, &b))
        {
            snprintf(err, err_len, "value for '%s' is not a boolean: '%s'", key, value);
            return false;
        }
        *(bool *) (base + def->offset) = negate ? !b : b;
        if (def->offset == offsetof(dm_config_t, do_register))
            cfg->register_explicit = true;
        return true;
    }

    case OPT_KEY:
    {
        int c;
        if (!dm_parse_key_spec(value, &c))
        {
            snprintf(err, err_len, "value for '%s' is not a character: '%s' "
                                   "(try '+', '^]', 0x1b or none)",
                     key, value ? value : "");
            return false;
        }
        *(int *) (base + def->offset) = c;
        return true;
    }

    case OPT_LOGLEVEL:
    {
        dm_log_level_t lvl;
        if (!dm_log_level_parse(value, &lvl))
        {
            snprintf(err, err_len, "unknown log level '%s'", value ? value : "");
            return false;
        }
        *(dm_log_level_t *) (base + def->offset) = lvl;
        return true;
    }
    }
    snprintf(err, err_len, "internal: unhandled option kind for '%s'", key);
    return false;
}

void dm_config_apply_env(dm_config_t *cfg)
{
    char env_name[128];
    char err[256];

    for (size_t i = 0; i < N_OPTS; i++)
    {
        const char *v;
        size_t n = (size_t) snprintf(env_name, sizeof(env_name), "DATAMODEM_%s", OPTS[i].name);
        if (n >= sizeof(env_name))
            continue;
        for (char *p = env_name; *p != '\0'; p++)
            *p = (*p == '-') ? '_' : (char) toupper((unsigned char) *p);

        v = getenv(env_name);
        if (v == NULL || *v == '\0')
            continue;
        if (!dm_config_set(cfg, OPTS[i].name, v, err, sizeof(err)))
            DM_WARN("config", "ignoring %s: %s", env_name, err);
    }
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

bool dm_config_apply_file(dm_config_t *cfg, const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int lineno = 0;

    if (f == NULL)
    {
        snprintf(err, err_len, "cannot read '%s': %s", path, strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *key;
        char *value;
        char *eq;

        lineno++;
        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        eq = strchr(key, '=');
        if (eq == NULL)
        {
            snprintf(err, err_len, "%s:%d: expected key=value", path, lineno);
            fclose(f);
            return false;
        }
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);
        /* Tolerate leading dashes so a config file can be copy-pasted flags. */
        while (*key == '-')
            key++;
        if (!dm_config_set(cfg, key, value, err, err_len))
        {
            char detail[512];
            snprintf(detail, sizeof(detail), "%s:%d: %s", path, lineno, err);
            snprintf(err, err_len, "%s", detail);
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

static bool command_from_string(const char *s, dm_command_t *out)
{
    if (strcmp(s, "dial") == 0 || strcmp(s, "call") == 0)
        *out = DM_CMD_DIAL;
    else if (strcmp(s, "answer") == 0 || strcmp(s, "listen") == 0)
        *out = DM_CMD_ANSWER;
    else if (strcmp(s, "selftest") == 0)
        *out = DM_CMD_SELFTEST;
    else if (strcmp(s, "version") == 0 || strcmp(s, "--version") == 0 || strcmp(s, "-V") == 0)
        *out = DM_CMD_VERSION;
    else if (strcmp(s, "help") == 0 || strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0)
        *out = DM_CMD_HELP;
    else
        return false;
    return true;
}

int dm_config_parse_args(dm_config_t *cfg, int argc, char *const argv[])
{
    struct option *longopts;
    char err[512];
    size_t n_long = 0;
    const char *positional = NULL;
    int first_arg;

    if (argc < 2)
    {
        cfg->command = DM_CMD_HELP;
        return DM_EXIT_USAGE;
    }

    if (command_from_string(argv[1], &cfg->command))
    {
        first_arg = 2;
    }
    else
    {
        /* The headline case: `datamodem <number>`. Anything that is not one
         * of the verbs is a number (or a SIP URI) to dial, so argv[1] falls
         * through to the positional handling below. */
        cfg->command = DM_CMD_DIAL;
        first_arg = 1;
    }
    if (cfg->command == DM_CMD_HELP || cfg->command == DM_CMD_VERSION)
        return DM_EXIT_OK;

    /* Environment first; explicit flags below win. */
    dm_config_apply_env(cfg);

    /* Two entries per bool (--x / --no-x), plus --config, --help, --verbose
     * and the terminator. */
    longopts = calloc(N_OPTS * 2 + 4, sizeof(*longopts));
    if (longopts == NULL)
        return DM_EXIT_INTERNAL;

    for (size_t i = 0; i < N_OPTS; i++)
    {
        longopts[n_long].name = OPTS[i].name;
        longopts[n_long].has_arg = (OPTS[i].kind == OPT_BOOL) ? optional_argument : required_argument;
        longopts[n_long].flag = NULL;
        longopts[n_long].val = 1000 + (int) i;
        n_long++;
        if (OPTS[i].kind == OPT_BOOL)
        {
            char *neg = malloc(strlen(OPTS[i].name) + 4);
            if (neg == NULL)
            {
                free(longopts);
                return DM_EXIT_INTERNAL;
            }
            sprintf(neg, "no-%s", OPTS[i].name);
            longopts[n_long].name = neg; /* leaked deliberately: lives for the process */
            longopts[n_long].has_arg = no_argument;
            longopts[n_long].flag = NULL;
            longopts[n_long].val = 2000 + (int) i;
            n_long++;
        }
    }
    longopts[n_long].name = "config";
    longopts[n_long].has_arg = required_argument;
    longopts[n_long].val = 'c';
    n_long++;
    longopts[n_long].name = "help";
    longopts[n_long].has_arg = no_argument;
    longopts[n_long].val = 'h';
    n_long++;
    longopts[n_long].name = "verbose";
    longopts[n_long].has_arg = no_argument;
    longopts[n_long].val = 'v';
    n_long++;

    optind = first_arg;
    for (;;)
    {
        int idx = 0;
        /* '+' makes getopt stop at the first non-option argument; we collect
         * it and resume, so flags may appear before or after the number
         * regardless of whether this libc permutes argv. */
        int c = getopt_long(argc, argv, "+c:hv", longopts, &idx);

        if (c == -1)
        {
            if (optind < argc && argv[optind][0] != '-')
            {
                if (positional == NULL)
                {
                    positional = argv[optind];
                }
                else
                {
                    fprintf(stderr, "datamodem: unexpected argument '%s'\n", argv[optind]);
                    free(longopts);
                    return DM_EXIT_USAGE;
                }
                optind++;
                continue;
            }
            break;
        }

        if (c == 'h')
        {
            free(longopts);
            cfg->command = DM_CMD_HELP;
            return DM_EXIT_OK;
        }
        if (c == 'v')
        {
            if (cfg->log_level < DM_LOG_TRACE)
                cfg->log_level++;
            continue;
        }
        if (c == 'c')
        {
            if (!dm_config_apply_file(cfg, optarg, err, sizeof(err)))
            {
                fprintf(stderr, "datamodem: %s\n", err);
                free(longopts);
                return DM_EXIT_CONFIG;
            }
            continue;
        }
        if (c >= 2000)
        {
            const opt_def_t *def = &OPTS[c - 2000];
            if (!dm_config_set(cfg, def->name, "false", err, sizeof(err)))
            {
                fprintf(stderr, "datamodem: %s\n", err);
                free(longopts);
                return DM_EXIT_USAGE;
            }
            continue;
        }
        if (c >= 1000)
        {
            const opt_def_t *def = &OPTS[c - 1000];
            const char *value = optarg;
            if (def->kind == OPT_BOOL && value == NULL)
                value = "true";
            if (!dm_config_set(cfg, def->name, value, err, sizeof(err)))
            {
                fprintf(stderr, "datamodem: %s\n", err);
                free(longopts);
                return DM_EXIT_USAGE;
            }
            continue;
        }
        /* getopt_long already printed the diagnostic. */
        free(longopts);
        return DM_EXIT_USAGE;
    }

    if (positional == NULL && optind < argc)
        positional = argv[optind++];
    if (optind < argc)
    {
        fprintf(stderr, "datamodem: unexpected argument '%s'\n", argv[optind]);
        free(longopts);
        return DM_EXIT_USAGE;
    }

    if (positional != NULL && cfg->command == DM_CMD_DIAL && cfg->to[0] == '\0')
        snprintf(cfg->to, sizeof(cfg->to), "%s", positional);
    else if (positional != NULL)
    {
        fprintf(stderr, "datamodem: unexpected argument '%s'\n", positional);
        free(longopts);
        return DM_EXIT_USAGE;
    }

    /* Registration only earns its keep when someone has to be able to call
     * us. Dialling needs credentials, not a registration: the trunk
     * challenges the INVITE and pjsip answers it. */
    if (!cfg->register_explicit)
        cfg->do_register = (cfg->command == DM_CMD_ANSWER);

    free(longopts);
    return DM_EXIT_OK;
}

static bool needs_sip(dm_command_t cmd)
{
    return cmd == DM_CMD_DIAL || cmd == DM_CMD_ANSWER;
}

bool dm_config_validate(const dm_config_t *cfg, char *err, size_t err_len)
{
    dm_modem_params_t params;

    if (needs_sip(cfg->command))
    {
        if (cfg->server[0] == '\0')
        {
            snprintf(err, err_len, "--server is required (or DATAMODEM_SERVER)");
            return false;
        }
        if (cfg->username[0] == '\0' && cfg->from_uri[0] == '\0')
        {
            snprintf(err, err_len, "--username is required (or DATAMODEM_USERNAME)");
            return false;
        }
        if (cfg->do_register && cfg->password[0] == '\0')
        {
            snprintf(err, err_len,
                     "--password is required when registering (or DATAMODEM_PASSWORD); "
                     "use --no-register for IP-authenticated trunks");
            return false;
        }
        if (strcasecmp(cfg->transport, "udp") != 0 && strcasecmp(cfg->transport, "tcp") != 0 &&
            strcasecmp(cfg->transport, "tls") != 0)
        {
            snprintf(err, err_len, "--transport must be udp, tcp or tls");
            return false;
        }
        if (strcasecmp(cfg->codec, "pcmu") != 0 && strcasecmp(cfg->codec, "pcma") != 0)
        {
            snprintf(err, err_len, "--codec must be pcmu or pcma");
            return false;
        }
    }

    if (cfg->command == DM_CMD_DIAL && cfg->to[0] == '\0')
    {
        snprintf(err, err_len, "no number to dial; usage: datamodem <number> [options]");
        return false;
    }

    /* One source of truth for what the modem will accept. */
    dm_modem_params_from_config(cfg, cfg->command != DM_CMD_ANSWER, "check", &params);
    if (!dm_modem_params_check(&params, err, err_len))
        return false;

    if (cfg->escape_char >= 0 && cfg->escape_char == cfg->escape_key)
    {
        snprintf(err, err_len, "--escape-key and --escape-char cannot be the same character");
        return false;
    }
    return true;
}

void dm_config_log(const dm_config_t *cfg)
{
    if (!dm_log_enabled(DM_LOG_DEBUG))
        return;
    DM_DEBUG("config",
             "server=%s username=%s auth_user=%s password=%s register=%s transport=%s local_port=%d",
             cfg->server, cfg->username, cfg->auth_user[0] ? cfg->auth_user : cfg->username,
             cfg->password[0] ? "<set>" : "<unset>", cfg->do_register ? "yes" : "no", cfg->transport,
             cfg->local_port);
    DM_DEBUG("config",
             "modulation=%s bit_rate=%d guard=%s format=%d%c%d v14=%s codec=%s jitter_buffer_ms=%d",
             cfg->modulation, cfg->bit_rate, cfg->guard_tone, cfg->data_bits,
             cfg->parity[0] == 'e' || cfg->parity[0] == 'E'
                 ? 'E'
                 : (cfg->parity[0] == 'o' || cfg->parity[0] == 'O' ? 'O' : 'N'),
             cfg->stop_bits, cfg->v14 ? "on" : "off", cfg->codec, cfg->jitter_buffer_ms);
    DM_DEBUG("config", "v42=%s v42_timeout=%d v42bis=%s dict=%d max_string=%d", cfg->v42,
             cfg->v42_timeout_s, cfg->v42bis ? "on" : "off", cfg->v42bis_dict, cfg->v42bis_max_string);
    DM_DEBUG("config", "escape_char=%d escape_guard_ms=%d escape_key=%d idle_timeout=%d media_timeout=%d "
                       "max_call=%d train_timeout=%d",
             cfg->escape_char, cfg->escape_guard_ms, cfg->escape_key, cfg->idle_timeout_s,
             cfg->media_timeout_s, cfg->max_call_s, cfg->train_timeout_s);
}

static void print_options(void)
{
    printf("Options. Every flag also reads DATAMODEM_<FLAG_IN_CAPS> from the environment,\n"
           "and any of them can be set in a --config file as key=value.\n\n");
    for (size_t i = 0; i < N_OPTS; i++)
    {
        const char *arg = "";
        int pad;

        switch (OPTS[i].kind)
        {
        case OPT_STR:
            arg = " <value>";
            break;
        case OPT_INT:
            arg = " <n>";
            break;
        case OPT_KEY:
            arg = " <char>";
            break;
        case OPT_LOGLEVEL:
            arg = " <level>";
            break;
        case OPT_BOOL:
            arg = "";
            break;
        }
        pad = 24 - (int) strlen(OPTS[i].name) - (int) strlen(arg);
        if (pad < 1)
            pad = 1;
        printf("  --%s%s%*s%s\n", OPTS[i].name, arg, pad, "", OPTS[i].help);
    }
    printf("  --config <path>         read key=value settings from a file\n");
    printf("  -v, --verbose           raise log verbosity (repeatable)\n");
    printf("  -h, --help              this text\n");
}

void dm_usage(void)
{
    printf("datamodem %s - a softmodem for data calls over SIP (V.21, Bell 103, V.23)\n\n",
           DATAMODEM_VERSION);
    printf("Usage:\n"
           "  datamodem <number> [options]      dial it and hand the terminal to the far modem\n"
           "  datamodem dial <number> [options] the same thing, spelled out\n"
           "  datamodem answer [options]        answer one inbound call and do the same\n"
           "  datamodem selftest [options]      loop two modems back to back, no SIP\n"
           "  datamodem version\n\n");
    printf("While connected, your terminal is the serial port: every keystroke goes out on\n"
           "the line, and everything the remote system sends lands on stdout. Escape back to\n"
           "the local command prompt with the Hayes sequence - pause, +++, pause - and then\n"
           "ATH to hang up, ATO to go back online, ATI for the session statistics.\n\n");
    printf("Logs go to stderr so that stdout carries only the remote system's output:\n"
           "  datamodem 5551234 > session.log      keeps a clean transcript\n"
           "  datamodem 5551234 --log-file dm.log  keeps the terminal clean\n\n");
    printf("Modulations: v32bis (14400 down to 4800), v32 (9600/4800), v22bis (2400),\n"
           "v22 (1200), v23 (1200 down / 75 up), v21 and bell103 (300). The default is v21\n"
           "because 300 bps FSK gets through an audio path that QAM will not; use\n"
           "--modulation v32bis, v32 or v22bis when the line is good. v32bis also talks to\n"
           "a far end that is only V.32, at 9600.\n\n");
    printf("V.42 error correction and V.42bis compression are available on top of any of\n"
           "them, and are off by default because the V.42 handshake puts junk on the line\n"
           "when the far end does not answer it:\n"
           "  --v42 detect     try V.42, fall back to direct async if nobody answers\n"
           "  --v42 require    try V.42, give up on the call if nobody answers\n"
           "  --v42bis         offer compression too (needs --v42); the far end may\n"
           "                   decline it, or take less than --v42bis-dict and\n"
           "                   --v42bis-max-string offer\n\n");
    printf("Examples:\n"
           "  export DATAMODEM_PASSWORD=...\n"
           "  datamodem +15551234567 --server sip.example.com --username 1001\n\n"
           "  datamodem 5551234 --server sip.example.com --username 1001 \\\n"
           "      --modulation bell103 --data-bits 7 --parity even\n\n"
           "  datamodem 5551234 --server sip.example.com --username 1001 \\\n"
           "      --v42 detect --v42bis\n\n"
           "  echo -e 'help\\r' | datamodem 5551234 --server sip.example.com --username 1001\n\n"
           "  datamodem answer --server sip.example.com --username 1001 --password ...\n\n"
           "  datamodem selftest --modulation v32bis\n\n");
    print_options();
}
