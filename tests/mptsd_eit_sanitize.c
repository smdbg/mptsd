/* Bounded EIT parsing under malformed transport packets. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "eit_forward.h"
static unsigned emitted;
static void emit(void *unused, const uint8_t *p, size_t size) {
    (void)unused;
    assert(size && size <= EIT_PACKETS_MAX && !(size % 188));
    uint8_t section[4096]; size_t copied = 0, total = 0;
    for (size_t i = 0; i < size; i += 188) {
        assert(p[i] == 0x47 && p[i+2] == 0x12);
        size_t off = i ? 4 : 5;
        if (!i) total = 3 + ((p[off+1] & 15) << 8) + p[off+2];
        size_t n = total - copied;
        if (n > 188 - off) n = 188 - off;
        memcpy(section + copied, p + i + off, n); copied += n;
    }
    assert(copied == total && eit_crc32(section, total) == 0);
    emitted++;
}
int main(void) {
    EIT_FORWARD state = {0};
    uint8_t s[4096]; memset(s, 0x67, sizeof(s));
    s[0] = 0x50; s[1] = 0xbf; s[2] = 0xfd; s[3] = 0; s[4] = 201;
    uint32_t crc = eit_crc32(s, sizeof(s) - 4);
    for (int i = 0; i < 4; i++) s[4092+i] = crc >> (24 - 8*i);
    size_t pos = 0; int cc = 0;
    while (pos < sizeof(s)) {
        uint8_t p[188]; memset(p, 0xff, sizeof(p));
        p[0] = 0x47; p[1] = pos ? 0 : 0x40; p[2] = 18; p[3] = 0x10 | (cc++ & 15);
        unsigned off = pos ? 4 : 5; p[4] = 0;
        size_t n = sizeof(s) - pos; if (n > 188-off) n = 188-off;
        memcpy(p+off, s+pos, n); pos += n;
        eit_forward_packet(&state, p, 201, 1001, 5311, 99, emit, NULL);
    }
    assert(emitted == 1);
    uint32_t seed = 0x819ac;
    for (int iteration = 0; iteration < 80000; iteration++) {
        uint8_t p[188];
        for (unsigned i = 0; i < sizeof(p); i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; p[i] = seed;
        }
        p[0] = 0x47; p[1] &= 0xc0; p[2] = 18; p[3] &= 0x3f;
        eit_forward_packet(&state, p, 201, 1001, 5311, 99, emit, NULL);
    }
    return 0;
}
