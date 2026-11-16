/* Low-cost sent-packet occupancy accounting. GPL-2.0, matching mptsd.
 * Owned by the existing output writer thread: no UDP listener, scan or lock. */
#include "output_status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>

static int64_t clock_ns(clockid_t clock_id) {
    struct timespec now;
    if (clock_gettime(clock_id, &now)) return 0;
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

void output_status_start(OUTPUT_STATUS *s) {
    s->last_sample_ns = s->last_attempt_ns = clock_ns(CLOCK_MONOTONIC);
}

void output_status_account(OUTPUT_STATUS *s, int complete_frame_written, unsigned null_packets) {
    if (!s->path) return;
    if (!complete_frame_written) { s->write_errors++; return; }
    s->total_packets += 7;
    s->null_packets += null_packets;
}

int output_status_publish_at(OUTPUT_STATUS *s, int64_t monotonic_ns, int64_t unix_ms) {
    if (!s->path || monotonic_ns - s->last_attempt_ns < (int64_t)s->interval_ms * 1000000)
        return 0;
    s->last_attempt_ns = monotonic_ns;
    uint64_t total = s->total_packets - s->last_total_packets;
    uint64_t nulls = s->null_packets - s->last_null_packets;
    double elapsed = (double)(monotonic_ns - s->last_sample_ns) / 1000000000;
    if (elapsed <= 0 || nulls > total) { errno = EINVAL; return -1; }
    double factor = 188.0 * 8 / elapsed / 1000000;
    char occupancy[40];
    if (total) snprintf(occupancy, sizeof(occupancy), "%.6f", (double)(total - nulls) * 100 / total);
    else strcpy(occupancy, "null");

    char *tmp = malloc(strlen(s->path) + sizeof(".tmp.XXXXXX"));
    if (!tmp) return -1;
    sprintf(tmp, "%s.tmp.XXXXXX", s->path);
    int fd = mkstemp(tmp); /* mode0600, same directory as target */
    if (fd < 0) { free(tmp); return -1; }
    FILE *file = fdopen(fd, "w");
    if (!file) { int saved = errno; close(fd); unlink(tmp); free(tmp); errno = saved; return -1; }
    int result = fprintf(file,
        "{\"pid\":%ld,\"sequence\":%" PRIu64 ",\"sample_unix_ms\":%" PRId64 ","
        "\"window_seconds\":%.6f,\"total_packets\":%" PRIu64 ",\"null_packets\":%" PRIu64 ","
        "\"window_total_packets\":%" PRIu64 ",\"window_null_packets\":%" PRIu64 ","
        "\"output_mbps\":%.6f,\"non_null_mbps\":%.6f,\"null_mbps\":%.6f,"
        "\"occupancy_pct\":%s,\"write_errors\":%" PRIu64 "}\n",
        (long)getpid(), s->sequence + 1, unix_ms, elapsed, s->total_packets, s->null_packets,
        total, nulls, total * factor, (total - nulls) * factor, nulls * factor,
        occupancy, s->write_errors);
    if (fclose(file)) result = -1;
    if (result >= 0 && rename(tmp, s->path) == 0) {
        s->sequence++;
        s->last_sample_ns = monotonic_ns;
        s->last_total_packets = s->total_packets;
        s->last_null_packets = s->null_packets;
        free(tmp);
        return 1;
    }
    int saved = errno;
    unlink(tmp); free(tmp); errno = saved;
    return -1;
}

int output_status_maybe_publish(OUTPUT_STATUS *s) {
    if (!s->path) return 0;
    int64_t now = clock_ns(CLOCK_MONOTONIC);
    if (now - s->last_attempt_ns < (int64_t)s->interval_ms * 1000000) return 0;
    return output_status_publish_at(s, now, clock_ns(CLOCK_REALTIME) / 1000000);
}
