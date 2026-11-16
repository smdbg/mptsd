/* Preserve full DVB EIT sections and all event descriptors while remapping IDs.
 * GPL-2.0-or-later, matching mptsd. No event parsing or regeneration is needed. */
#include "eit_forward.h"
#include <string.h>

uint32_t eit_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffff;
    while (len--) {
        crc ^= (uint32_t)*data++ << 24;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
    }
    return crc;
}

static void forward_section(EIT_FORWARD *s, uint16_t input_sid,
    uint16_t output_sid, uint16_t tsid, uint16_t onid,
    eit_emit_fn emit, void *opaque) {
    uint8_t *d = s->section;
    size_t len = s->needed;
    /* Only actual-TS p/f and schedule belonging to this input's PAT service.
     * Other-TS tables must not be relabelled as belonging to the output TS. */
    if (len < 18 || !(d[1] & 0x80) ||
        !(d[0] == 0x4e || (d[0] >= 0x50 && d[0] <= 0x5f)) ||
        !input_sid || (((unsigned)d[3] << 8) | d[4]) != input_sid ||
        eit_crc32(d, len) != 0)
        return;
    d[3] = output_sid >> 8; d[4] = output_sid;
    d[8] = tsid >> 8; d[9] = tsid;
    d[10] = onid >> 8; d[11] = onid;
    uint32_t crc = eit_crc32(d, len - 4);
    for (int i = 0; i < 4; i++) d[len - 4 + i] = crc >> (24 - i * 8);

    uint8_t packets[EIT_PACKETS_MAX];
    size_t copied = 0, count = 0;
    while (copied < len) {
        uint8_t *p = packets + count * 188;
        memset(p, 0xff, 188);
        p[0] = 0x47; p[1] = count ? 0 : 0x40; p[2] = 0x12; p[3] = 0x10;
        size_t offset = count ? 4 : 5;
        if (!count) p[4] = 0;
        size_t take = len - copied;
        if (take > 188 - offset) take = 188 - offset;
        memcpy(p + offset, d + copied, take);
        copied += take;
        count++;
    }
    /* One atomic queue insertion keeps sections from multiple inputs separate.
     * The single mux thread assigns the output PID 0x12 continuity counter. */
    emit(opaque, packets, count * 188);
}

static void consume(EIT_FORWARD *s, const uint8_t *data, size_t len,
    int can_start, uint16_t input_sid, uint16_t output_sid,
    uint16_t tsid, uint16_t onid, eit_emit_fn emit, void *opaque) {
    while (len) {
        if (!s->used && (!can_start || *data == 0xff)) return;
        size_t target = s->used < 3 ? 3 : s->needed;
        size_t take = target - s->used;
        if (take > len) take = len;
        memcpy(s->section + s->used, data, take);
        s->used += take; data += take; len -= take;
        if (s->used == 3) {
            s->needed = 3 + (((unsigned)s->section[1] & 15) << 8) + s->section[2];
            if (s->needed < 18 || s->needed > EIT_SECTION_MAX) {
                s->used = s->needed = 0;
                return;
            }
        }
        if (s->needed && s->used == s->needed) {
            forward_section(s, input_sid, output_sid, tsid, onid, emit, opaque);
            s->used = s->needed = 0;
            if (!can_start) return;
        }
    }
}

void eit_forward_packet(EIT_FORWARD *s, const uint8_t p[188],
    uint16_t input_sid, uint16_t output_sid, uint16_t tsid, uint16_t onid,
    eit_emit_fn emit, void *opaque) {
    if (p[0] != 0x47 || (p[1] & 0x80) || (p[3] & 0xc0) ||
        (((unsigned)(p[1] & 31) << 8) | p[2]) != 0x12 || !(p[3] & 0x30)) {
        s->used = s->needed = 0; s->cc_valid = 0; return;
    }
    size_t offset = 4;
    if (p[3] & 0x20) {
        offset += 1 + p[4];
        if (offset > 188) { s->used = s->needed = 0; s->cc_valid = 0; return; }
        if (p[4] && (p[5] & 0x80)) {
            s->used = s->needed = 0; s->cc_valid = 0;
        }
    }
    if (!(p[3] & 0x10) || offset >= 188) return;
    int cc = p[3] & 15;
    if (s->cc_valid) {
        if (cc == s->cc && !memcmp(s->last_packet, p, 188)) return;
        if (cc != ((s->cc + 1) & 15)) s->used = s->needed = 0;
    }
    s->cc = cc; s->cc_valid = 1;
    memcpy(s->last_packet, p, 188);
    if (p[1] & 0x40) {
        unsigned pointer = p[offset++];
        if (pointer > 188 - offset) { s->used = s->needed = 0; return; }
        if (s->used && pointer)
            consume(s, p + offset, pointer, 0, input_sid, output_sid, tsid, onid, emit, opaque);
        /* An incomplete section at the new-section boundary was lost. */
        s->used = s->needed = 0;
        offset += pointer;
        consume(s, p + offset, 188 - offset, 1, input_sid, output_sid, tsid, onid, emit, opaque);
    } else if (s->used) {
        consume(s, p + offset, 188 - offset, 0, input_sid, output_sid, tsid, onid, emit, opaque);
    }
}
