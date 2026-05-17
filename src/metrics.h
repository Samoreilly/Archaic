#pragma once
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    atomic_uint_least64_t queries_total;
    atomic_uint_least64_t completions_total;
    atomic_uint_least64_t scans_total;
    atomic_uint_least64_t errors_total;
    atomic_uint_least64_t cache_hits;
    atomic_uint_least64_t cache_misses;
    atomic_uint_least64_t query_latency_ns_sum;
    atomic_uint_least64_t query_latency_ns_count;
} metrics_t;

void metrics_init(metrics_t* m);
void metrics_record_query(metrics_t* m, uint64_t latency_ns);
void metrics_record_completion(metrics_t* m);
void metrics_record_scan(metrics_t* m);
void metrics_record_error(metrics_t* m);
void metrics_record_cache_hit(metrics_t* m);
void metrics_record_cache_miss(metrics_t* m);

typedef struct {
    uint64_t queries_total;
    uint64_t completions_total;
    uint64_t scans_total;
    uint64_t errors_total;
    uint64_t cache_hits;
    uint64_t cache_misses;
    double query_latency_avg_ms;
} metrics_snapshot;

metrics_snapshot metrics_snapshot_get(metrics_t* m);
