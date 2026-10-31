#include "capture.h"
#include "rmon.h"
#include "snmp_tables.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stop_requested;

static void
handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

int
main(int argc, char **argv)
{
    struct rmon_state state;
    pthread_t capture_thread;
    pthread_t timer_thread;
    int capture_started = 0;
    int timer_started = 0;

    if (argc != 2 || argv[1][0] == '\0' || strchr(argv[1], '/') != NULL) {
        fprintf(stderr, "usage: %s <ethernet-interface>\n", argv[0]);
        return 2;
    }
    if (rmon_state_init(&state, argv[1]) != 0) {
        fprintf(stderr, "cannot initialize interface %s\n", argv[1]);
        return 2;
    }
    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    netsnmp_enable_subagent();
    init_agent("rmon-probe");
    rmon_register_tables(&state);
    init_snmp("rmon-probe");

    if (pthread_create(&capture_thread, NULL, rmon_capture_thread, &state) == 0)
        capture_started = 1;
    else
        fprintf(stderr, "cannot start capture thread\n");
    if (pthread_create(&timer_thread, NULL, rmon_history_timer, &state) == 0)
        timer_started = 1;
    else
        fprintf(stderr, "cannot start history timer thread\n");

    while (!stop_requested)
        (void)agent_check_and_process(1);

    pthread_mutex_lock(&state.lock);
    state.stopping = true;
    pthread_mutex_unlock(&state.lock);
    if (capture_started)
        (void)pthread_join(capture_thread, NULL);
    if (timer_started)
        (void)pthread_join(timer_thread, NULL);
    snmp_shutdown("rmon-probe");
    rmon_state_destroy(&state);
    return 0;
}
