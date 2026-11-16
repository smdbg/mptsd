/* Automatic conditional-access PID remultiplexing. GPL-2.0-or-later. */
#ifndef CAS_REMUX_H
#define CAS_REMUX_H
#include <stdint.h>
#include <sys/time.h>
struct input;
struct output;
struct ca_input;
struct ca_output;
void ca_input_init(struct input *r);
void ca_input_reset(struct input *r);
void ca_input_free(struct input *r);
void ca_input_tick(struct input *r);
void ca_pmt_packet(struct input *r, const uint8_t packet[188]);
void ca_cat_packet(struct input *r, const uint8_t packet[188]);
uint16_t ca_packet_pid(struct input *r, uint16_t pid);
int ca_shared_emm_packet(struct input *r, uint16_t pid, const uint8_t packet[188]);
void ca_output_tick(struct output *o, struct timeval *now);
void ca_output_free(struct output *o);
#endif
