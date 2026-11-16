/* Input EIT forwarding. GPL-2.0-or-later, matching mptsd. */
#ifndef EIT_FORWARD_H
#define EIT_FORWARD_H
#include <stdint.h>
#include <stddef.h>
#define EIT_SECTION_MAX 4096
#define EIT_PACKETS_MAX (23 * 188)
typedef struct {
    uint8_t section[EIT_SECTION_MAX];
    size_t used, needed;
    uint8_t last_packet[188];
    int cc_valid, cc;
} EIT_FORWARD;
typedef void (*eit_emit_fn)(void *, const uint8_t *, size_t);
uint32_t eit_crc32(const uint8_t *data, size_t len);
void eit_forward_packet(EIT_FORWARD *state, const uint8_t packet[188],
    uint16_t input_sid, uint16_t output_sid, uint16_t tsid, uint16_t onid,
    eit_emit_fn emit, void *opaque);
#endif
