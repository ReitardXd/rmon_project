#include "rmon.h"

#include <limits.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const oid ifindex_oid[] = {1, 3, 6, 1, 2, 1, 2, 2, 1, 1};

static struct timespec
timespec_add_seconds(struct timespec value, long seconds)
{
    value.tv_sec += seconds;
    return value;
}

static int
timespec_compare(const struct timespec *left, const struct timespec *right)
{
    if (left->tv_sec != right->tv_sec)
        return left->tv_sec < right->tv_sec ? -1 : 1;
    if (left->tv_nsec != right->tv_nsec)
        return left->tv_nsec < right->tv_nsec ? -1 : 1;
    return 0;
}

int
rmon_state_init(struct rmon_state *state, const char *ifname)
{
    size_t length;

    if (state == NULL || ifname == NULL)
        return -1;
    length = strlen(ifname);
    if (length == 0 || length >= sizeof(state->ifname))
        return -1;
    memset(state, 0, sizeof(*state));
    memcpy(state->ifname, ifname, length + 1);
    state->ifindex = if_nametoindex(ifname);
    if (state->ifindex == 0)
        return -1;
    if (pthread_mutex_init(&state->lock, NULL) != 0)
        return -1;
    (void)clock_gettime(CLOCK_MONOTONIC, &state->started_at);
    return 0;
}

void
rmon_state_destroy(struct rmon_state *state)
{
    struct rmon_stats_row *stats;
    struct rmon_history_row *history;

    if (state == NULL)
        return;
    pthread_mutex_lock(&state->lock);
    while ((stats = state->stats_rows) != NULL) {
        state->stats_rows = stats->next;
        free(stats);
    }
    while ((history = state->history_rows) != NULL) {
        state->history_rows = history->next;
        free(history->samples);
        free(history);
    }
    pthread_mutex_unlock(&state->lock);
    pthread_mutex_destroy(&state->lock);
}

unsigned long
rmon_uptime_ticks(const struct rmon_state *state)
{
    struct timespec now;
    time_t seconds;
    long nanoseconds;
    unsigned long long ticks;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    seconds = now.tv_sec - state->started_at.tv_sec;
    nanoseconds = now.tv_nsec - state->started_at.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000L;
    }
    ticks = (unsigned long long)seconds * 100ULL +
        (unsigned long long)nanoseconds / 10000000ULL;
    return ticks > ULONG_MAX ? ULONG_MAX : (unsigned long)ticks;
}

void
rmon_counter_add_packet(struct rmon_state *state, const unsigned char *packet,
    uint32_t captured_length, uint32_t wire_length)
{
    struct rmon_counters *counter;
    bool broadcast = false;
    bool multicast = false;

    if (state == NULL)
        return;
    if (captured_length >= 6) {
        broadcast = memcmp(packet, "\377\377\377\377\377\377", 6) == 0;
        multicast = !broadcast && (packet[0] & 1U) != 0;
    }
    pthread_mutex_lock(&state->lock);
    counter = &state->counters;
    counter->pkts++;
    counter->octets += wire_length;
    if (broadcast)
        counter->broadcast_pkts++;
    else if (multicast)
        counter->multicast_pkts++;
    if (wire_length < 64)
        counter->undersize_pkts++;
    else if (wire_length > 1518)
        counter->oversize_pkts++;
    else if (wire_length == 64)
        counter->pkts_64++;
    else if (wire_length <= 127)
        counter->pkts_65_127++;
    else if (wire_length <= 255)
        counter->pkts_128_255++;
    else if (wire_length <= 511)
        counter->pkts_256_511++;
    else if (wire_length <= 1023)
        counter->pkts_512_1023++;
    else
        counter->pkts_1024_1518++;
    pthread_mutex_unlock(&state->lock);
}

void
rmon_set_capture_drops(struct rmon_state *state, uint64_t drops)
{
    pthread_mutex_lock(&state->lock);
    state->counters.drop_events = drops;
    pthread_mutex_unlock(&state->lock);
}

void
rmon_set_interface_errors(struct rmon_state *state, uint64_t crc, uint64_t collisions)
{
    pthread_mutex_lock(&state->lock);
    if (!state->interface_stats_seen) {
        state->last_crc_raw = crc;
        state->last_collisions_raw = collisions;
        state->interface_stats_seen = true;
    } else {
        if (crc >= state->last_crc_raw)
            state->counters.crc_align_errors += crc - state->last_crc_raw;
        if (collisions >= state->last_collisions_raw)
            state->counters.collisions += collisions - state->last_collisions_raw;
        state->last_crc_raw = crc;
        state->last_collisions_raw = collisions;
    }
    pthread_mutex_unlock(&state->lock);
}

struct rmon_counters
rmon_counter_delta(const struct rmon_counters *now, const struct rmon_counters *before)
{
    struct rmon_counters value;
#define DELTA(member) value.member = now->member - before->member
    DELTA(drop_events); DELTA(octets); DELTA(pkts); DELTA(broadcast_pkts);
    DELTA(multicast_pkts); DELTA(crc_align_errors); DELTA(undersize_pkts);
    DELTA(oversize_pkts); DELTA(fragments); DELTA(jabbers); DELTA(collisions);
    DELTA(pkts_64); DELTA(pkts_65_127); DELTA(pkts_128_255); DELTA(pkts_256_511);
    DELTA(pkts_512_1023); DELTA(pkts_1024_1518);
#undef DELTA
    return value;
}

bool
rmon_source_is_capture_interface(const struct rmon_state *state,
    const oid *source, size_t source_len)
{
    return source_len == OID_LENGTH(ifindex_oid) + 1 &&
        memcmp(source, ifindex_oid, sizeof(ifindex_oid)) == 0 &&
        source[source_len - 1] == state->ifindex;
}

static long
history_utilization(const struct rmon_state *state, const struct rmon_counters *delta,
    long seconds)
{
    long double bits;
    long double percent_hundredths;

    if (state->link_bps == 0 || seconds <= 0)
        return 0;
    bits = (long double)delta->octets * 8.0L + (long double)delta->pkts * 160.0L;
    percent_hundredths = bits * 10000.0L /
        ((long double)state->link_bps * (long double)seconds);
    return percent_hundredths >= 10000.0L ? 10000L : (long)percent_hundredths;
}

static void
history_append_locked(struct rmon_state *state, struct rmon_history_row *row)
{
    struct rmon_history_sample *sample;
    struct rmon_counters now = state->counters;
    size_t slot;

    if (row->samples == NULL || row->buckets_granted <= 0)
        return;
    if (row->sample_count < (size_t)row->buckets_granted) {
        slot = (row->sample_head + row->sample_count) % (size_t)row->buckets_granted;
        row->sample_count++;
    } else {
        slot = row->sample_head;
        row->sample_head = (row->sample_head + 1) % (size_t)row->buckets_granted;
    }
    sample = &row->samples[slot];
    sample->control_index = row->index;
    sample->sample_index = row->next_sample_index++;
    sample->interval_start = row->interval_start;
    sample->counters = rmon_counter_delta(&now, &row->interval_baseline);
    sample->utilization = history_utilization(state, &sample->counters, row->interval_seconds);
    row->interval_baseline = now;
    row->interval_start = rmon_uptime_ticks(state);
}

void *
rmon_history_timer(void *argument)
{
    struct rmon_state *state = argument;
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};

    for (;;) {
        struct rmon_history_row *row;
        struct timespec now;

        (void)nanosleep(&pause, NULL);
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        pthread_mutex_lock(&state->lock);
        if (state->stopping) {
            pthread_mutex_unlock(&state->lock);
            break;
        }
        for (row = state->history_rows; row != NULL; row = row->next) {
            if (row->status != RMON_STATUS_VALID ||
                timespec_compare(&now, &row->next_deadline) < 0)
                continue;
            history_append_locked(state, row);
            row->next_deadline = timespec_add_seconds(row->next_deadline, row->interval_seconds);
        }
        pthread_mutex_unlock(&state->lock);
    }
    return NULL;
}
