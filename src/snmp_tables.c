#include "snmp_tables.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    STATS_INDEX = 1, STATS_SOURCE, STATS_DROPS, STATS_OCTETS, STATS_PKTS,
    STATS_BROADCAST, STATS_MULTICAST, STATS_CRC, STATS_UNDERSIZE,
    STATS_OVERSIZE, STATS_FRAGMENTS, STATS_JABBERS, STATS_COLLISIONS,
    STATS_64, STATS_65_127, STATS_128_255, STATS_256_511, STATS_512_1023,
    STATS_1024_1518, STATS_OWNER, STATS_STATUS
};
enum { HC_INDEX = 1, HC_SOURCE, HC_REQUESTED, HC_GRANTED, HC_INTERVAL, HC_OWNER, HC_STATUS };
enum { EH_INDEX = 1, EH_SAMPLE, EH_START, EH_DROPS, EH_OCTETS, EH_PKTS,
       EH_BROADCAST, EH_MULTICAST, EH_CRC, EH_UNDERSIZE, EH_OVERSIZE,
       EH_FRAGMENTS, EH_JABBERS, EH_COLLISIONS, EH_UTILIZATION };

struct history_key { long index; long sample; };
static struct rmon_state *rmon;

static void set_integer(netsnmp_variable_list *vb, long value)
{ snmp_set_var_typed_integer(vb, ASN_INTEGER, value); }
static void set_counter(netsnmp_variable_list *vb, uint64_t value)
{ u_long low = (u_long)(uint32_t)value; snmp_set_var_typed_value(vb, ASN_COUNTER, (u_char *)&low, sizeof(low)); }
static void set_ticks(netsnmp_variable_list *vb, unsigned long value)
{ u_long ticks = value; snmp_set_var_typed_value(vb, ASN_TIMETICKS, (u_char *)&ticks, sizeof(ticks)); }

static struct rmon_stats_row *stats_find_locked(long index)
{
    struct rmon_stats_row *row;
    for (row = rmon->stats_rows; row != NULL; row = row->next)
        if (row->index == index) return row;
    return NULL;
}

static struct rmon_history_row *history_find_locked(long index)
{
    struct rmon_history_row *row;
    for (row = rmon->history_rows; row != NULL; row = row->next)
        if (row->index == index) return row;
    return NULL;
}

static struct rmon_stats_row *stats_copy_next(long after)
{
    struct rmon_stats_row *row, *best = NULL;
    pthread_mutex_lock(&rmon->lock);
    for (row = rmon->stats_rows; row != NULL; row = row->next)
        if (row->index > after && (best == NULL || row->index < best->index)) best = row;
    pthread_mutex_unlock(&rmon->lock);
    return best;
}

static netsnmp_variable_list *stats_iterate(void **loop, void **data,
    netsnmp_variable_list *indexes, bool first)
{
    long *key = *loop;
    struct rmon_stats_row *copy;
    if (first) {
        key = calloc(1, sizeof(*key));
        if (key == NULL) return NULL;
        *loop = key;
    }
    copy = stats_copy_next(*key);
    if (copy == NULL) return NULL;
    *key = copy->index;
    snmp_set_var_typed_integer(indexes, ASN_INTEGER, copy->index);
    *data = copy;
    return indexes;
}
static netsnmp_variable_list *stats_first(void **l, void **d, netsnmp_variable_list *i, netsnmp_iterator_info *x)
{ (void)x; return stats_iterate(l, d, i, true); }
static netsnmp_variable_list *stats_next(void **l, void **d, netsnmp_variable_list *i, netsnmp_iterator_info *x)
{ (void)x; return stats_iterate(l, d, i, false); }

static struct rmon_history_sample *history_copy_next(long after_index, long after_sample)
{
    struct rmon_history_row *row;
    struct rmon_history_sample *best = NULL;
    size_t position;
    pthread_mutex_lock(&rmon->lock);
    for (row = rmon->history_rows; row != NULL; row = row->next) {
        for (position = 0; position < row->sample_count; position++) {
            struct rmon_history_sample *candidate = &row->samples[
                (row->sample_head + position) % (size_t)row->buckets_granted];
            if (candidate->control_index < after_index ||
                (candidate->control_index == after_index && candidate->sample_index <= after_sample))
                continue;
            if (best == NULL || candidate->control_index < best->control_index ||
                (candidate->control_index == best->control_index && candidate->sample_index < best->sample_index))
                best = candidate;
        }
    }
    pthread_mutex_unlock(&rmon->lock);
    return best;
}

static netsnmp_variable_list *history_iterate(void **loop, void **data,
    netsnmp_variable_list *indexes, bool first)
{
    struct history_key *key = *loop;
    struct rmon_history_sample *copy;
    if (first) {
        key = calloc(1, sizeof(*key));
        if (key == NULL) return NULL;
        *loop = key;
    }
    copy = history_copy_next(key->index, key->sample);
    if (copy == NULL) return NULL;
    key->index = copy->control_index;
    key->sample = copy->sample_index;
    snmp_set_var_typed_integer(indexes, ASN_INTEGER, copy->control_index);
    snmp_set_var_typed_integer(indexes->next_variable, ASN_INTEGER, copy->sample_index);
    *data = copy;
    return indexes;
}
static netsnmp_variable_list *history_first(void **l, void **d, netsnmp_variable_list *i, netsnmp_iterator_info *x)
{ (void)x; return history_iterate(l, d, i, true); }
static netsnmp_variable_list *history_next(void **l, void **d, netsnmp_variable_list *i, netsnmp_iterator_info *x)
{ (void)x; return history_iterate(l, d, i, false); }

static struct rmon_history_row *control_copy_next(long after)
{
    struct rmon_history_row *row, *best = NULL;
    pthread_mutex_lock(&rmon->lock);
    for (row = rmon->history_rows; row != NULL; row = row->next)
        if (row->index > after && (best == NULL || row->index < best->index)) best = row;
    pthread_mutex_unlock(&rmon->lock);
    return best;
}
static netsnmp_variable_list *control_iterate(void **loop, void **data,
    netsnmp_variable_list *indexes, bool first)
{
    long *key = *loop;
    struct rmon_history_row *copy;
    if (first) { key = calloc(1, sizeof(*key)); if (key == NULL) return NULL; *loop = key; }
    copy = control_copy_next(*key);
    if (copy == NULL) return NULL;
    *key = copy->index;
    snmp_set_var_typed_integer(indexes, ASN_INTEGER, copy->index);
    *data = copy;
    return indexes;
}
static netsnmp_variable_list *control_first(void **l, void **d, netsnmp_variable_list *i, netsnmp_iterator_info *x)
{ (void)x; return control_iterate(l, d, i, true); }
static netsnmp_variable_list *control_next(void **l, void **d, netsnmp_variable_list *i, netsnmp_iterator_info *x)
{ (void)x; return control_iterate(l, d, i, false); }

static void stats_get(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    netsnmp_request_info *request;
    for (request = requests; request != NULL; request = request->next) {
        struct rmon_stats_row *row = netsnmp_extract_iterator_context(request);
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        struct rmon_counters value;
        if (row == NULL) { netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE); continue; }
        pthread_mutex_lock(&rmon->lock);
        value = rmon_counter_delta(&rmon->counters, &row->baseline);
        pthread_mutex_unlock(&rmon->lock);
        switch (info->colnum) {
        case STATS_INDEX: set_integer(request->requestvb, row->index); break;
        case STATS_SOURCE: snmp_set_var_typed_value(request->requestvb, ASN_OBJECT_ID,
            (u_char *)row->data_source, row->data_source_len * sizeof(oid)); break;
        case STATS_DROPS: set_counter(request->requestvb, value.drop_events); break;
        case STATS_OCTETS: set_counter(request->requestvb, value.octets); break;
        case STATS_PKTS: set_counter(request->requestvb, value.pkts); break;
        case STATS_BROADCAST: set_counter(request->requestvb, value.broadcast_pkts); break;
        case STATS_MULTICAST: set_counter(request->requestvb, value.multicast_pkts); break;
        case STATS_CRC: set_counter(request->requestvb, value.crc_align_errors); break;
        case STATS_UNDERSIZE: set_counter(request->requestvb, value.undersize_pkts); break;
        case STATS_OVERSIZE: set_counter(request->requestvb, value.oversize_pkts); break;
        case STATS_FRAGMENTS: set_counter(request->requestvb, value.fragments); break;
        case STATS_JABBERS: set_counter(request->requestvb, value.jabbers); break;
        case STATS_COLLISIONS: set_counter(request->requestvb, value.collisions); break;
        case STATS_64: set_counter(request->requestvb, value.pkts_64); break;
        case STATS_65_127: set_counter(request->requestvb, value.pkts_65_127); break;
        case STATS_128_255: set_counter(request->requestvb, value.pkts_128_255); break;
        case STATS_256_511: set_counter(request->requestvb, value.pkts_256_511); break;
        case STATS_512_1023: set_counter(request->requestvb, value.pkts_512_1023); break;
        case STATS_1024_1518: set_counter(request->requestvb, value.pkts_1024_1518); break;
        case STATS_OWNER: snmp_set_var_typed_value(request->requestvb, ASN_OCTET_STR,
            (u_char *)row->owner, strlen(row->owner)); break;
        case STATS_STATUS: set_integer(request->requestvb, row->status); break;
        default: netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT); break;
        }
    }
}

static void control_get(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    netsnmp_request_info *request;
    for (request = requests; request != NULL; request = request->next) {
        struct rmon_history_row *row = netsnmp_extract_iterator_context(request);
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        if (row == NULL) { netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE); continue; }
        switch (info->colnum) {
        case HC_INDEX: set_integer(request->requestvb, row->index); break;
        case HC_SOURCE: snmp_set_var_typed_value(request->requestvb, ASN_OBJECT_ID,
            (u_char *)row->data_source, row->data_source_len * sizeof(oid)); break;
        case HC_REQUESTED: set_integer(request->requestvb, row->buckets_requested); break;
        case HC_GRANTED: set_integer(request->requestvb, row->buckets_granted); break;
        case HC_INTERVAL: set_integer(request->requestvb, row->interval_seconds); break;
        case HC_OWNER: snmp_set_var_typed_value(request->requestvb, ASN_OCTET_STR,
            (u_char *)row->owner, strlen(row->owner)); break;
        case HC_STATUS: set_integer(request->requestvb, row->status); break;
        default: netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT); break;
        }
    }
}

static void history_get(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    netsnmp_request_info *request;
    for (request = requests; request != NULL; request = request->next) {
        struct rmon_history_sample *row = netsnmp_extract_iterator_context(request);
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        struct rmon_history_sample sample;
        if (row == NULL) { netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE); continue; }
        pthread_mutex_lock(&rmon->lock);
        sample = *row;
        pthread_mutex_unlock(&rmon->lock);
        switch (info->colnum) {
        case EH_INDEX: set_integer(request->requestvb, sample.control_index); break;
        case EH_SAMPLE: set_integer(request->requestvb, sample.sample_index); break;
        case EH_START: set_ticks(request->requestvb, sample.interval_start); break;
        case EH_DROPS: set_counter(request->requestvb, sample.counters.drop_events); break;
        case EH_OCTETS: set_counter(request->requestvb, sample.counters.octets); break;
        case EH_PKTS: set_counter(request->requestvb, sample.counters.pkts); break;
        case EH_BROADCAST: set_counter(request->requestvb, sample.counters.broadcast_pkts); break;
        case EH_MULTICAST: set_counter(request->requestvb, sample.counters.multicast_pkts); break;
        case EH_CRC: set_counter(request->requestvb, sample.counters.crc_align_errors); break;
        case EH_UNDERSIZE: set_counter(request->requestvb, sample.counters.undersize_pkts); break;
        case EH_OVERSIZE: set_counter(request->requestvb, sample.counters.oversize_pkts); break;
        case EH_FRAGMENTS: set_counter(request->requestvb, sample.counters.fragments); break;
        case EH_JABBERS: set_counter(request->requestvb, sample.counters.jabbers); break;
        case EH_COLLISIONS: set_counter(request->requestvb, sample.counters.collisions); break;
        case EH_UTILIZATION: set_integer(request->requestvb, sample.utilization); break;
        default: netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHOBJECT); break;
        }
    }
}

static long request_index(netsnmp_request_info *request)
{
    netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
    return (info == NULL || info->indexes == NULL) ? -1 : *info->indexes->val.integer;
}

static long requested_status(netsnmp_request_info *requests, long index, unsigned int status_column)
{
    netsnmp_request_info *request;
    for (request = requests; request != NULL; request = request->next) {
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        if (info != NULL && request_index(request) == index && info->colnum == status_column &&
            request->requestvb->type == ASN_INTEGER) return *request->requestvb->val.integer;
    }
    return 0;
}

static int resize_history_locked(struct rmon_history_row *row, long requested)
{
    struct rmon_history_sample *samples;
    size_t granted = (size_t)(requested > (long)RMON_MAX_BUCKETS ? RMON_MAX_BUCKETS : requested);
    size_t copy_count, start, offset;
    if (granted == 0) return -1;
    samples = calloc(granted, sizeof(*samples));
    if (samples == NULL) return -1;
    copy_count = row->sample_count < granted ? row->sample_count : granted;
    start = row->sample_count > copy_count ? row->sample_count - copy_count : 0;
    for (offset = 0; offset < copy_count; offset++)
        samples[offset] = row->samples[(row->sample_head + start + offset) %
            (row->buckets_granted == 0 ? 1U : (size_t)row->buckets_granted)];
    free(row->samples);
    row->samples = samples;
    row->sample_head = 0;
    row->sample_count = copy_count;
    row->buckets_requested = requested;
    row->buckets_granted = (long)granted;
    return 0;
}

static struct rmon_stats_row *create_stats_locked(long index)
{
    struct rmon_stats_row *row = calloc(1, sizeof(*row));
    if (row == NULL) return NULL;
    row->index = index; row->status = RMON_STATUS_UNDER_CREATION;
    row->next = rmon->stats_rows; rmon->stats_rows = row;
    return row;
}

static struct rmon_history_row *create_history_locked(long index)
{
    struct rmon_history_row *row = calloc(1, sizeof(*row));
    if (row == NULL) return NULL;
    row->index = index; row->status = RMON_STATUS_UNDER_CREATION;
    row->interval_seconds = 1800; row->next_sample_index = 1;
    if (resize_history_locked(row, 50) != 0) { free(row); return NULL; }
    row->next = rmon->history_rows; rmon->history_rows = row;
    return row;
}

static void remove_stats_locked(long index)
{
    struct rmon_stats_row **link = &rmon->stats_rows;
    while (*link != NULL) {
        if ((*link)->index == index) { struct rmon_stats_row *old = *link; *link = old->next; free(old); return; }
        link = &(*link)->next;
    }
}
static void remove_history_locked(long index)
{
    struct rmon_history_row **link = &rmon->history_rows;
    while (*link != NULL) {
        if ((*link)->index == index) { struct rmon_history_row *old = *link; *link = old->next; free(old->samples); free(old); return; }
        link = &(*link)->next;
    }
}

static bool valid_source_vb(netsnmp_variable_list *vb)
{
    return vb->type == ASN_OBJECT_ID && vb->val_len % sizeof(oid) == 0 &&
        rmon_source_is_capture_interface(rmon, vb->val.objid, vb->val_len / sizeof(oid));
}

static int reserve_write(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests,
    bool history)
{
    netsnmp_request_info *request;
    unsigned int source = history ? HC_SOURCE : STATS_SOURCE;
    unsigned int owner = history ? HC_OWNER : STATS_OWNER;
    unsigned int status = history ? HC_STATUS : STATS_STATUS;
    unsigned int requested = history ? HC_REQUESTED : 0;
    unsigned int interval = history ? HC_INTERVAL : 0;
    for (request = requests; request != NULL; request = request->next) {
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        long index = request_index(request);
        if (index < 1 || index > 65535 || info == NULL) {
            netsnmp_set_request_error(reqinfo, request, SNMP_ERR_INCONSISTENTNAME); return -1;
        }
        if (info->colnum == source && !valid_source_vb(request->requestvb)) {
            netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGVALUE); return -1;
        }
        if (info->colnum == owner && (request->requestvb->type != ASN_OCTET_STR ||
            request->requestvb->val_len >= RMON_OWNER_MAX)) {
            netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGLENGTH); return -1;
        }
        if ((info->colnum == requested && (request->requestvb->type != ASN_INTEGER ||
            *request->requestvb->val.integer < 1 || *request->requestvb->val.integer > 65535)) ||
            (info->colnum == interval && (request->requestvb->type != ASN_INTEGER ||
            *request->requestvb->val.integer < 1 || *request->requestvb->val.integer > 3600)) ||
            (info->colnum == status && (request->requestvb->type != ASN_INTEGER ||
            *request->requestvb->val.integer < 1 || *request->requestvb->val.integer > 4))) {
            netsnmp_set_request_error(reqinfo, request, SNMP_ERR_WRONGVALUE); return -1;
        }
        if (info->colnum != source && info->colnum != owner && info->colnum != status &&
            info->colnum != requested && info->colnum != interval) {
            netsnmp_set_request_error(reqinfo, request, SNMP_ERR_NOTWRITABLE); return -1;
        }
    }
    return 0;
}

static void stats_set(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    netsnmp_request_info *request;
    if (reqinfo->mode == MODE_SET_RESERVE1) { (void)reserve_write(reqinfo, requests, false); return; }
    if (reqinfo->mode != MODE_SET_ACTION) return;
    pthread_mutex_lock(&rmon->lock);
    for (request = requests; request != NULL; request = request->next) {
        long index = request_index(request);
        if (stats_find_locked(index) == NULL && requested_status(requests, index, STATS_STATUS) == RMON_STATUS_CREATE_REQUEST &&
            create_stats_locked(index) == NULL) { netsnmp_set_request_error(reqinfo, request, SNMP_ERR_RESOURCEUNAVAILABLE); }
    }
    for (request = requests; request != NULL; request = request->next) {
        struct rmon_stats_row *row = stats_find_locked(request_index(request));
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        if (row == NULL) { netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE); continue; }
        if (row->status == RMON_STATUS_VALID && info->colnum != STATS_STATUS) { netsnmp_set_request_error(reqinfo, request, SNMP_ERR_INCONSISTENTVALUE); continue; }
        if (info->colnum == STATS_SOURCE) { row->data_source_len = request->requestvb->val_len / sizeof(oid); memcpy(row->data_source, request->requestvb->val.objid, request->requestvb->val_len); }
        else if (info->colnum == STATS_OWNER) { memcpy(row->owner, request->requestvb->val.string, request->requestvb->val_len); row->owner[request->requestvb->val_len] = '\0'; }
        else if (info->colnum == STATS_STATUS) {
            long status = *request->requestvb->val.integer;
            if (status == RMON_STATUS_VALID) {
                if (!rmon_source_is_capture_interface(rmon, row->data_source, row->data_source_len)) netsnmp_set_request_error(reqinfo, request, SNMP_ERR_INCONSISTENTVALUE);
                else { row->baseline = rmon->counters; row->status = RMON_STATUS_VALID; }
            } else if (status == RMON_STATUS_INVALID) remove_stats_locked(row->index);
            else row->status = RMON_STATUS_UNDER_CREATION;
        }
    }
    pthread_mutex_unlock(&rmon->lock);
}

static void history_set(netsnmp_agent_request_info *reqinfo, netsnmp_request_info *requests)
{
    netsnmp_request_info *request;
    if (reqinfo->mode == MODE_SET_RESERVE1) { (void)reserve_write(reqinfo, requests, true); return; }
    if (reqinfo->mode != MODE_SET_ACTION) return;
    pthread_mutex_lock(&rmon->lock);
    for (request = requests; request != NULL; request = request->next) {
        long index = request_index(request);
        if (history_find_locked(index) == NULL && requested_status(requests, index, HC_STATUS) == RMON_STATUS_CREATE_REQUEST &&
            create_history_locked(index) == NULL) netsnmp_set_request_error(reqinfo, request, SNMP_ERR_RESOURCEUNAVAILABLE);
    }
    for (request = requests; request != NULL; request = request->next) {
        struct rmon_history_row *row = history_find_locked(request_index(request));
        netsnmp_table_request_info *info = netsnmp_extract_table_info(request);
        if (row == NULL) { netsnmp_set_request_error(reqinfo, request, SNMP_NOSUCHINSTANCE); continue; }
        if (row->status == RMON_STATUS_VALID && info->colnum != HC_STATUS) { netsnmp_set_request_error(reqinfo, request, SNMP_ERR_INCONSISTENTVALUE); continue; }
        if (info->colnum == HC_SOURCE) { row->data_source_len = request->requestvb->val_len / sizeof(oid); memcpy(row->data_source, request->requestvb->val.objid, request->requestvb->val_len); }
        else if (info->colnum == HC_REQUESTED && resize_history_locked(row, *request->requestvb->val.integer) != 0) netsnmp_set_request_error(reqinfo, request, SNMP_ERR_RESOURCEUNAVAILABLE);
        else if (info->colnum == HC_INTERVAL) row->interval_seconds = *request->requestvb->val.integer;
        else if (info->colnum == HC_OWNER) { memcpy(row->owner, request->requestvb->val.string, request->requestvb->val_len); row->owner[request->requestvb->val_len] = '\0'; }
        else if (info->colnum == HC_STATUS) {
            long status = *request->requestvb->val.integer;
            if (status == RMON_STATUS_VALID) {
                if (!rmon_source_is_capture_interface(rmon, row->data_source, row->data_source_len)) netsnmp_set_request_error(reqinfo, request, SNMP_ERR_INCONSISTENTVALUE);
                else { struct timespec now; row->interval_baseline = rmon->counters; row->interval_start = rmon_uptime_ticks(rmon); (void)clock_gettime(CLOCK_MONOTONIC, &now); now.tv_sec += row->interval_seconds; row->next_deadline = now; row->status = RMON_STATUS_VALID; }
            } else if (status == RMON_STATUS_INVALID) remove_history_locked(row->index);
            else row->status = RMON_STATUS_UNDER_CREATION;
        }
    }
    pthread_mutex_unlock(&rmon->lock);
}

static int stats_handler(netsnmp_mib_handler *h, netsnmp_handler_registration *r,
    netsnmp_agent_request_info *ri, netsnmp_request_info *q)
{ (void)h; (void)r; if (ri->mode == MODE_GET) stats_get(ri, q); else stats_set(ri, q); return SNMP_ERR_NOERROR; }
static int control_handler(netsnmp_mib_handler *h, netsnmp_handler_registration *r,
    netsnmp_agent_request_info *ri, netsnmp_request_info *q)
{ (void)h; (void)r; if (ri->mode == MODE_GET) control_get(ri, q); else history_set(ri, q); return SNMP_ERR_NOERROR; }
static int history_handler(netsnmp_mib_handler *h, netsnmp_handler_registration *r,
    netsnmp_agent_request_info *ri, netsnmp_request_info *q)
{ (void)h; (void)r; if (ri->mode == MODE_GET) history_get(ri, q); return SNMP_ERR_NOERROR; }

static void free_iterator_context(void *context, netsnmp_iterator_info *info)
{
    (void)info;
    free(context);
}

static void register_iterator(const char *name, const oid *base, size_t base_len,
    Netsnmp_Node_Handler *handler, int writable, int indexes, int max_column,
    Netsnmp_First_Data_Point *first, Netsnmp_Next_Data_Point *next)
{
    netsnmp_handler_registration *registration;
    netsnmp_table_registration_info *table;
    netsnmp_iterator_info *iterator;
    registration = netsnmp_create_handler_registration(name, handler, base, base_len,
        writable ? HANDLER_CAN_RWRITE : HANDLER_CAN_RONLY);
    table = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);
    iterator = SNMP_MALLOC_TYPEDEF(netsnmp_iterator_info);
    if (registration == NULL || table == NULL || iterator == NULL) return;
    if (indexes == 2)
        netsnmp_table_helper_add_indexes(table, ASN_INTEGER, ASN_INTEGER, 0);
    else
        netsnmp_table_helper_add_indexes(table, ASN_INTEGER, 0);
    table->min_column = 1; table->max_column = max_column;
    iterator->get_first_data_point = first; iterator->get_next_data_point = next;
    iterator->free_loop_context_at_end = free_iterator_context;
    iterator->table_reginfo = table;
    netsnmp_register_table_iterator2(registration, iterator);
}

void rmon_register_tables(struct rmon_state *state)
{
    static const oid stats_oid[] = {1,3,6,1,2,1,16,1,1};
    static const oid control_oid[] = {1,3,6,1,2,1,16,2,1};
    static const oid history_oid[] = {1,3,6,1,2,1,16,2,2};
    rmon = state;
    register_iterator("etherStatsTable", stats_oid, OID_LENGTH(stats_oid), stats_handler, 1, 1, STATS_STATUS, stats_first, stats_next);
    register_iterator("historyControlTable", control_oid, OID_LENGTH(control_oid), control_handler, 1, 1, HC_STATUS, control_first, control_next);
    register_iterator("etherHistoryTable", history_oid, OID_LENGTH(history_oid), history_handler, 0, 2, EH_UTILIZATION, history_first, history_next);
}

static void set_source(oid *dst, size_t *len, unsigned int ifindex)
{
    static const oid base[] = {1,3,6,1,2,1,2,2,1,1};
    memcpy(dst, base, sizeof(base));
    dst[OID_LENGTH(base)] = ifindex;
    *len = OID_LENGTH(base) + 1;
}

void rmon_bootstrap_rows(void)
{
    static const long hb[][2] = {{120, 30}, {50, 1800}};
    struct rmon_stats_row *s;
    struct rmon_history_row *h;
    struct timespec now;
    size_t i;
    pthread_mutex_lock(&rmon->lock);
    if ((s = create_stats_locked(1)) != NULL) {
        set_source(s->data_source, &s->data_source_len, rmon->ifindex);
        (void)snprintf(s->owner, sizeof(s->owner), "bootstrap");
        s->baseline = rmon->counters;
        s->status = RMON_STATUS_VALID;
    }
    for (i = 0; i < 2; i++) {
        if ((h = create_history_locked((long)i + 1)) == NULL) continue;
        set_source(h->data_source, &h->data_source_len, rmon->ifindex);
        (void)resize_history_locked(h, hb[i][0]);
        h->interval_seconds = hb[i][1];
        (void)snprintf(h->owner, sizeof(h->owner), "bootstrap");
        h->interval_baseline = rmon->counters;
        h->interval_start = rmon_uptime_ticks(rmon);
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        now.tv_sec += h->interval_seconds;
        h->next_deadline = now;
        h->status = RMON_STATUS_VALID;
    }
    pthread_mutex_unlock(&rmon->lock);
}
