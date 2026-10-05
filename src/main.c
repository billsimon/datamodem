#include "datamodem/build_info.h"
#include "datamodem/config.h"
#include "datamodem/log.h"
#include "datamodem/modem.h"
#include "datamodem/session.h"
#include "datamodem/sip.h"
#include "datamodem/term.h"
#include "datamodem/tty.h"
#include "datamodem/util.h"
#include "datamodem/version.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void) sig;
    g_stop = 1;
    /* Raw mode is the one thing that must not survive us, and the session
     * loop may not get another turn if the signal arrived at a bad moment. */
    dm_tty_restore();
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* A closed stdout must not kill a call in progress: the session loop
     * notices the failed write and clears the call properly. */
    signal(SIGPIPE, SIG_IGN);
}

/* Shared by dial and answer: raw terminal, run the session, clear the call. */
static int run_session(const dm_config_t *cfg, dm_modem_t *modem)
{
    dm_session_result_t sr;
    dm_call_result_t cr;
    char err[256];
    int rc;

    if (!dm_tty_raw(err, sizeof(err)))
    {
        DM_ERROR("tty", "cannot put the terminal into raw mode: %s", err);
        dm_sip_hangup(&cr);
        return DM_EXIT_INTERNAL;
    }

    rc = dm_session_run(cfg, modem, &g_stop, &sr);

    dm_tty_restore();
    if (dm_term_active())
        dm_tty_quiet(); /* the screen is still ours until we are done */
    dm_term_state("HANGING UP", "%s", dm_session_end_name(sr.reason));
    dm_sip_hangup(&cr);
    dm_term_end(dm_session_end_name(sr.reason));

    dm_log_event(DM_LOG_INFO, "call", "cleared", "remote=\"%s\" sip_status=%d sip_reason=\"%s\" "
                                                 "duration_ms=%d",
                 cr.remote_uri, cr.sip_status, cr.sip_reason, cr.duration_ms);
    return rc;
}

static int cmd_dial(const dm_config_t *cfg)
{
    dm_modem_params_t params;
    dm_modem_t *modem;
    int rc;

    dm_modem_params_from_config(cfg, true, "out", &params);
    modem = dm_modem_create(&params);
    if (modem == NULL)
        return DM_EXIT_CONFIG;

    rc = dm_sip_start(cfg);
    if (rc != DM_EXIT_OK)
    {
        dm_modem_destroy(modem);
        return rc;
    }

    rc = dm_sip_dial(cfg, cfg->to, modem, &g_stop);
    if (rc == DM_EXIT_OK)
        rc = run_session(cfg, modem);
    else
        dm_sip_hangup(NULL);

    dm_sip_stop();
    dm_modem_destroy(modem);
    return rc;
}

static int cmd_answer(const dm_config_t *cfg)
{
    dm_modem_params_t params;
    dm_modem_t *modem;
    int rc;

    dm_modem_params_from_config(cfg, false, "in", &params);
    modem = dm_modem_create(&params);
    if (modem == NULL)
        return DM_EXIT_CONFIG;

    rc = dm_sip_start(cfg);
    if (rc != DM_EXIT_OK)
    {
        dm_modem_destroy(modem);
        return rc;
    }

    rc = dm_sip_answer(cfg, modem, &g_stop);
    if (rc == DM_EXIT_OK)
        rc = run_session(cfg, modem);
    else
        dm_sip_hangup(NULL);

    dm_sip_stop();
    dm_modem_destroy(modem);
    return rc;
}

int main(int argc, char *argv[])
{
    dm_config_t cfg;
    char err[512];
    int rc;

    dm_config_defaults(&cfg);
    dm_log_init(cfg.log_level, cfg.log_json);

    rc = dm_config_parse_args(&cfg, argc, argv);

    if (cfg.command == DM_CMD_HELP)
    {
        dm_usage();
        return rc;
    }
    if (cfg.command == DM_CMD_VERSION)
    {
        printf("datamodem %s (softmodem data pumps over SIP/G.711)\n", DATAMODEM_VERSION);
        printf("  build      %s, %s\n", DATAMODEM_BUILD_TYPE, DATAMODEM_BUILD_DATE);
        printf("  compiler   %s on %s\n", DATAMODEM_BUILD_COMPILER, DATAMODEM_BUILD_SYSTEM);
        printf("  spandsp    %s\n", DATAMODEM_SPANDSP_VERSION);
        printf("  pjproject  %s\n", DATAMODEM_PJPROJECT_VERSION);
        printf("  modems     V.32bis 14400-4800, V.32 9600/4800, V.22bis 2400/1200, V.22 1200,\n"
               "             V.21 300, Bell 103 300, V.23 1200/75\n");
        return DM_EXIT_OK;
    }

    dm_log_init(cfg.log_level, cfg.log_json);
    if (cfg.log_file[0] != '\0')
    {
        if (!dm_mkdir_parent(cfg.log_file, err, sizeof(err)) ||
            !dm_log_set_file(cfg.log_file, err, sizeof(err)))
        {
            fprintf(stderr, "datamodem: %s\n", err);
            return DM_EXIT_CONFIG;
        }
    }
    if (rc != DM_EXIT_OK)
        return rc;

    if (!dm_config_validate(&cfg, err, sizeof(err)))
    {
        DM_ERROR("config", "%s", err);
        DM_INFO("config", "run 'datamodem help' for the full list of options");
        return DM_EXIT_CONFIG;
    }

    install_signal_handlers();
    dm_modem_init_logging(&cfg);

    /* An interactive call takes the screen from here, so that everything
     * from registering onwards is on the status line rather than scrolling
     * past. */
    if (cfg.command == DM_CMD_DIAL || cfg.command == DM_CMD_ANSWER)
        dm_term_start(&cfg);

    DM_INFO("datamodem", "datamodem %s starting (%s build, spandsp %s, pjproject %s)", DATAMODEM_VERSION,
            DATAMODEM_BUILD_TYPE, DATAMODEM_SPANDSP_VERSION, DATAMODEM_PJPROJECT_VERSION);
    dm_config_log(&cfg);

    switch (cfg.command)
    {
    case DM_CMD_SELFTEST:
        rc = dm_modem_selftest(&cfg);
        break;
    case DM_CMD_DIAL:
        rc = cmd_dial(&cfg);
        break;
    case DM_CMD_ANSWER:
        rc = cmd_answer(&cfg);
        break;
    default:
        dm_usage();
        rc = DM_EXIT_USAGE;
        break;
    }

    DM_INFO("datamodem", "exit %d", rc);
    dm_term_stop();
    dm_log_close();
    return rc;
}
