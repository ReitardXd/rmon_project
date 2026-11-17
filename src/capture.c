#include "capture.h"

#include <pcap/pcap.h>

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint64_t
read_u64_file(const char *path, uint64_t fallback)
{
    FILE *file;
    unsigned long long value;

    file = fopen(path, "r");
    if (file == NULL)
        return fallback;
    if (fscanf(file, "%llu", &value) != 1) {
        (void)fclose(file);
        return fallback;
    }
    (void)fclose(file);
    return (uint64_t)value;
}

static void
read_interface_stats(struct rmon_state *state)
{
    char crc_path[160];
    char collisions_path[160];
    char speed_path[128];
    uint64_t crc;
    uint64_t collisions;
    uint64_t speed;

    (void)snprintf(crc_path, sizeof(crc_path),
        "/sys/class/net/%s/statistics/rx_crc_errors", state->ifname);
    (void)snprintf(collisions_path, sizeof(collisions_path),
        "/sys/class/net/%s/statistics/collisions", state->ifname);
    crc = read_u64_file(crc_path, 0);
    collisions = read_u64_file(collisions_path, 0);
    rmon_set_interface_errors(state, crc, collisions);

    (void)snprintf(speed_path, sizeof(speed_path), "/sys/class/net/%s/speed", state->ifname);
    speed = read_u64_file(speed_path, 0);
    if (speed > 0 && speed <= UINT64_MAX / 1000000ULL) {
        pthread_mutex_lock(&state->lock);
        state->link_bps = speed * 1000000ULL;
        pthread_mutex_unlock(&state->lock);
    }
}

static void
packet_callback(unsigned char *user, const struct pcap_pkthdr *header,
    const unsigned char *packet)
{
    struct rmon_state *state = (struct rmon_state *)user;
    uint32_t octets;

    if (header->len > UINT32_MAX - 4U)
        return;
    /* Linux packet sockets normally omit the FCS; RMON's lengths include it. */
    octets = header->len + 4U;
    rmon_counter_add_packet(state, packet, header->caplen, octets);
}

static void *
capture_fail(void)
{
    (void)kill(getpid(), SIGTERM);
    return NULL;
}

void *
rmon_capture_thread(void *argument)
{
    struct rmon_state *state = argument;
    char error[PCAP_ERRBUF_SIZE] = {0};
    pcap_t *handle;

    handle = pcap_create(state->ifname, error);
    if (handle == NULL) {
        fprintf(stderr, "pcap_create(%s): %s\n", state->ifname, error);
        return capture_fail();
    }
    if (pcap_set_snaplen(handle, 64) != 0 || pcap_set_promisc(handle, 1) != 0 ||
        pcap_set_timeout(handle, 250) != 0 ||
        pcap_set_buffer_size(handle, 16 * 1024 * 1024) != 0 || pcap_activate(handle) < 0) {
        fprintf(stderr, "pcap activation on %s failed: %s\n", state->ifname, pcap_geterr(handle));
        pcap_close(handle);
        return capture_fail();
    }
    if (pcap_datalink(handle) != DLT_EN10MB) {
        fprintf(stderr, "%s is not an Ethernet capture device (DLT %d)\n",
            state->ifname, pcap_datalink(handle));
        pcap_close(handle);
        return capture_fail();
    }
    if (pcap_setdirection(handle, PCAP_D_IN) != 0) {
        fprintf(stderr, "cannot restrict %s capture to inbound: %s\n", state->ifname, pcap_geterr(handle));
        pcap_close(handle);
        return capture_fail();
    }
    read_interface_stats(state);

    for (;;) {
        struct pcap_stat statistics;
        int result;
        bool stopping;

        pthread_mutex_lock(&state->lock);
        stopping = state->stopping;
        pthread_mutex_unlock(&state->lock);
        if (stopping)
            break;
        result = pcap_dispatch(handle, 64, packet_callback, (unsigned char *)state);
        if (result == PCAP_ERROR) {
            fprintf(stderr, "pcap_dispatch(%s): %s\n", state->ifname, pcap_geterr(handle));
            break;
        }
        if (pcap_stats(handle, &statistics) == 0)
            rmon_set_capture_drops(state, (uint64_t)statistics.ps_drop);
        read_interface_stats(state);
    }
    pcap_close(handle);
    return NULL;
}
