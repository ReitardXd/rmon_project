#ifndef RMON_H
#define RMON_H

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define RMON_OWNER_MAX 128
#define RMON_MAX_BUCKETS 4096U

enum rmon_entry_status {
    RMON_STATUS_VALID = 1,
    RMON_STATUS_CREATE_REQUEST = 2,
    RMON_STATUS_UNDER_CREATION = 3,
    RMON_STATUS_INVALID = 4
};

struct rmon_counters {
    uint64_t drop_events;
    uint64_t octets;
    uint64_t pkts;
    uint64_t broadcast_pkts;
    uint64_t multicast_pkts;
    uint64_t crc_align_errors;
    uint64_t undersize_pkts;
    uint64_t oversize_pkts;
    uint64_t fragments;
    uint64_t jabbers;
    uint64_t collisions;
    uint64_t pkts_64;
    uint64_t pkts_65_127;
    uint64_t pkts_128_255;
    uint64_t pkts_256_511;
    uint64_t pkts_512_1023;
    uint64_t pkts_1024_1518;
};

struct rmon_stats_row {
    long index;
    oid data_source[MAX_OID_LEN];
    size_t data_source_len;
    char owner[RMON_OWNER_MAX];
    int status;
    struct rmon_counters baseline;
    struct rmon_stats_row *next;
};

struct rmon_history_sample {
    long control_index;
    long sample_index;
    unsigned long interval_start;
    struct rmon_counters counters;
    long utilization;
};

struct rmon_history_row {
    long index;
    oid data_source[MAX_OID_LEN];
    size_t data_source_len;
    long buckets_requested;
    long buckets_granted;
    long interval_seconds;
    char owner[RMON_OWNER_MAX];
    int status;
    struct rmon_counters interval_baseline;
    unsigned long interval_start;
    struct timespec next_deadline;
    long next_sample_index;
    struct rmon_history_sample *samples;
    size_t sample_count;
    size_t sample_head;
    struct rmon_history_row *next;
};

struct rmon_state {
    pthread_mutex_t lock;
    bool stopping;
    char ifname[64];
    unsigned int ifindex;
    uint64_t link_bps;
    struct timespec started_at;
    struct rmon_counters counters;
    uint64_t last_crc_raw;
    uint64_t last_collisions_raw;
    bool interface_stats_seen;
    struct rmon_stats_row *stats_rows;
    struct rmon_history_row *history_rows;
};

int rmon_state_init(struct rmon_state *state, const char *ifname);
void rmon_state_destroy(struct rmon_state *state);
unsigned long rmon_uptime_ticks(const struct rmon_state *state);
void rmon_counter_add_packet(struct rmon_state *state, const unsigned char *packet,
    uint32_t captured_length, uint32_t wire_length);
void rmon_set_capture_drops(struct rmon_state *state, uint64_t drops);
void rmon_set_interface_errors(struct rmon_state *state, uint64_t crc, uint64_t collisions);
struct rmon_counters rmon_counter_delta(const struct rmon_counters *now,
    const struct rmon_counters *before);
bool rmon_source_is_capture_interface(const struct rmon_state *state,
    const oid *source, size_t source_len);

void *rmon_history_timer(void *argument);

#endif
