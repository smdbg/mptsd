/* Low-cost sent-packet occupancy accounting. GPL-2.0, matching mptsd. */
#ifndef OUTPUT_STATUS_H
#define OUTPUT_STATUS_H
#include <stdint.h>
typedef struct {
    char *path;
    unsigned interval_ms;
    int network_output;
    uint64_t total_packets, null_packets, write_errors, sequence;
    uint64_t last_total_packets, last_null_packets;
    int64_t last_sample_ns, last_attempt_ns;
} OUTPUT_STATUS;
void output_status_start(OUTPUT_STATUS *s);
void output_status_account(OUTPUT_STATUS *s, int complete_frame_written, unsigned null_packets);
int output_status_publish_at(OUTPUT_STATUS *s, int64_t monotonic_ns, int64_t unix_ms);
int output_status_maybe_publish(OUTPUT_STATUS *s);
#endif
