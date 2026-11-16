/* All-or-nothing ring insertion is needed to serialize EIT from many inputs. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include "libfuncs/cbuf.h"

#define RECORDS 5000
static void *produce(void *opaque) {
    CBUF *b = opaque;
    for (uint32_t sequence = 0; sequence < RECORDS; sequence++) {
        uint8_t packet[188];
        memset(packet, sequence & 255, sizeof(packet));
        memcpy(packet, &sequence, sizeof(sequence));
        while (cbuf_fill(b, packet, sizeof(packet)) < 0) sched_yield();
    }
    return NULL;
}

static void test_owned_dequeue(void) {
    CBUF *b = cbuf_init(3 * 188, "owned-psi");
    uint8_t initial[3 * 188], overwrite[188], owned[188], rest[2 * 188];
    memset(initial, 0x11, sizeof(initial));
    memset(overwrite, 0x22, sizeof(overwrite));
    assert(cbuf_fill(b, initial, sizeof(initial)) == 0);
    assert(cbuf_read(b, owned, sizeof(owned)) == sizeof(owned));
    /* Immediately reuse the consumed slot, exactly the former pointer race. */
    assert(cbuf_fill(b, overwrite, sizeof(overwrite)) == 0);
    for (int i = 0; i < 188; i++) assert(owned[i] == 0x11);
    assert(cbuf_read(b, rest, sizeof(rest)) == sizeof(rest));
    for (int i = 0; i < 2 * 188; i++) assert(rest[i] == 0x11);
    assert(cbuf_read(b, owned, sizeof(owned)) == sizeof(owned));
    for (int i = 0; i < 188; i++) assert(owned[i] == 0x22);
    assert(cbuf_read(b, owned, sizeof(owned)) == 0);
    cbuf_free(&b);

    /* A deliberately tiny, constantly wrapping PSI queue exercises producer
     * reuse while the consumer inspects its independent packet copy. */
    b = cbuf_init(3 * 188, "concurrent-psi");
    pthread_t producer;
    assert(pthread_create(&producer, NULL, produce, b) == 0);
    for (uint32_t expected = 0; expected < RECORDS; expected++) {
        int count;
        while (!(count = cbuf_read(b, owned, sizeof(owned)))) sched_yield();
        assert(count == sizeof(owned));
        sched_yield();
        uint32_t received;
        memcpy(&received, owned, sizeof(received));
        assert(received == expected);
        for (int i = sizeof(received); i < 188; i++) assert(owned[i] == (expected & 255));
    }
    assert(pthread_join(producer, NULL) == 0);
    cbuf_free(&b);
}

int main(void) {
    CBUF *b = cbuf_init(1000, "synthetic");
    uint8_t a[1000], c[1000]; memset(a, 0x11, sizeof(a)); memset(c, 0x22, sizeof(c));
    int got;
    assert(cbuf_fill(b, a, 900) == 0);
    assert(cbuf_get(b, 800, &got) && got == 800);
    uint64_t before = b->input;
    /* Overflow after wrapping must not publish a partial EIT section. */
    assert(cbuf_fill(b, c, 950) == -1);
    assert(b->input == before);
    assert(cbuf_fill(b, c, 800) == 0);
    uint8_t *data = cbuf_get(b, 200, &got);
    assert(got == 200);
    for (int i = 0; i < 100; i++) assert(data[i] == 0x11);
    for (int i = 100; i < 200; i++) assert(data[i] == 0x22);
    data = cbuf_get(b, 700, &got); assert(got == 700);
    for (int i = 0; i < got; i++) assert(data[i] == 0x22);
    assert(cbuf_fill(b, c, 1000) == 0);
    assert(cbuf_fill(b, a, 1) == -1);
    cbuf_free(&b);
    test_owned_dequeue();
    return 0;
}
