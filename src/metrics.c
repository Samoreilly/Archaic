#include "metrics.h"
#include <string.h>

void metrics_init(metrics_t* m) {
    if (!m) return;
    atomic_store(&m->queries_total, 0);
    atomic_store(&m->completions_total, 0);
    atomic_store(&m->scans_total, 0);
    atomic_store(&m->errors_total, 0);
    atomic_store(&m->cache_hits, 0);
    atomic_store(&m->cache_misses, 0);
    atomic_store(&m->query_latency_ns_sum, 0);
    atomic_store(&m->query_latency_ns_count, 0);
}

void metrics_record_query(metrics_t* m, uint64_t latency_ns) {
    if (!m) return;
    atomic_fetch_add(&m->queries_total, 1);
    atomic_fetch_add(&m->query_latency_ns_sum, latency_ns);
    atomic_fetch_add(&m->query_latency_ns_count, 1);
}

void metrics_record_completion(metrics_t* m) {
    if (!m) return;
    atomic_fetch_add(&m->completions_total, 1);
}

void metrics_record_scan(metrics_t* m) {
    if (!m) return;
    atomic_fetch_add(&m->scans_total, 1);
}

void metrics_record_error(metrics_t* m) {
    if (!m) return;
    atomic_fetch_add(&m->errors_total, 1);
}

void metrics_record_cache_hit(metrics_t* m) {
    if (!m) return;
    atomic_fetch_add(&m->cache_hits, 1);
}

void metrics_record_cache_miss(metrics_t* m) {
    if (!m) return;
    atomic_fetch_add(&m->cache_misses, 1);
}

metrics_snapshot metrics_snapshot_get(metrics_t* m) {
    metrics_snapshot s;
    memset(&s, 0, sizeof(s));
    if (!m) return s;

    s.queries_total = atomic_load(&m->queries_total);
    s.completions_total = atomic_load(&m->completions_total);
    s.scans_total = atomic_load(&m->scans_total);
    s.errors_total = atomic_load(&m->errors_total);
    s.cache_hits = atomic_load(&m->cache_hits);
    s.cache_misses = atomic_load(&m->cache_misses);

    uint64_t sum = atomic_load(&m->query_latency_ns_sum);
    uint64_t count = atomic_load(&m->query_latency_ns_count);
    if (count > 0) {
        s.query_latency_avg_ms = (double)sum / (double)count / 1000000.0;
    }
    return s;
}
