/* Automatic ECM/EMM remultiplexing. GPL-2.0-or-later, matching mptsd.
 * Input threads own parsing and maps; the PSI thread reads bounded published
 * snapshots under a per-input mutex. Shared EMM sections alone use a bounded
 * queue; audio/video forwarding keeps its lock-free map lookup.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include "data.h"
#include "config.h"
#include "libtsfuncs/tsfuncs.h"

extern CONFIG *config;
extern void input_buffer_add(INPUT *, uint8_t *, int);

#define CA_SLOTS 128
#define CA_BASE 0x1000
#define CA_CAT_MAX 4096
#define CA_SECTION_MAX 1024
#define CA_MERGED_MAX (31 * CA_CAT_MAX + 6) /* optional global EMM descriptor */
#define CA_CAT_SECTIONS 256
#define SHARED_BASE 8064
#define SHARED_GROUPS 32
#define EMM_MAX 4096
#define EMM_QUEUE 16

typedef struct { uint16_t size; uint8_t data[EMM_MAX]; } EMM_SECTION;

typedef struct {
    uint8_t data[EMM_MAX], last_packet[188];
    size_t used, needed;
    int cc_valid, cc, private_emm;
} CA_ASSEMBLER;

typedef struct {
    uint8_t *data[256];
    uint16_t size[256];
    int valid, version, last;
    size_t bytes;
} CA_PENDING;

struct ca_input {
    pthread_mutex_t lock;
    /* Only these published fields are read by the PSI thread. */
    uint8_t published_cat[CA_CAT_MAX];
    size_t published_size;
    int scrambled;
    /* Bounded complete-section queue; PSI thread is the sole shared PID writer. */
    EMM_SECTION emms[EMM_QUEUE];
    unsigned emm_head, emm_count;
    uint64_t emm_received_ms;
    uint16_t shared_output_pid;
    int shared_accepted, shared_conflict;

    CA_ASSEMBLER pmt_assembler, cat_assembler, emm_assembler;
    uint16_t shared_input_pid;
    CA_PENDING pending;
    uint8_t pmt[CA_SECTION_MAX], cat[CA_CAT_MAX];
    size_t pmt_size, cat_size;
    uint8_t deferred_pmt[CA_SECTION_MAX], deferred_cat[CA_CAT_MAX];
    size_t deferred_pmt_size, deferred_cat_size;
    int have_deferred_cat, deferred_cat_version, deferred_cat_last;
    uint32_t deferred_cat_crc[256];
    uint16_t slots[CA_SLOTS]; /* sticky for INPUT lifetime; never alias queued data */
    uint16_t map[8192];
    int pmt_version, have_version;
    int cat_version, cat_last, have_cat;
    uint32_t cat_crc[256];
    time_t last_error;
    uint64_t pmt_emitted_ms, deferred_pmt_ms, deferred_cat_ms;
};

typedef struct {
    size_t start, size;
} CA_SPAN;

struct ca_output {
    uint8_t descriptors[CA_MERGED_MAX];
    size_t size;
    CA_SPAN sections[CA_CAT_SECTIONS];
    unsigned count, cursor;
    uint8_t version, cc;
    int initialized;
    int shared_present;
    struct timeval refresh, cycle;
    unsigned emm_owner[SHARED_GROUPS];
    uint8_t emm_cc[SHARED_GROUPS];
    int emm_socket;
    uint64_t emm_retry_ms;
    time_t emm_last_error;
};

static uint64_t monotonic_ms(void);

static unsigned get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static void set16(uint8_t *p, unsigned value) { p[0] = value >> 8; p[1] = value; }
static unsigned get_ts_pid(const uint8_t *p) { return get16(p) & 8191; }
static void setpid(uint8_t *p, unsigned value) { p[0] = (p[0] & 0xe0) | (value >> 8); p[1] = value; }

static void error(INPUT *r, const char *message) {
    /* Invalid/repeated upstream tables must not turn into a logging hot loop. */
    time_t now = time(NULL);
    if (now != r->cas->last_error) {
        r->cas->last_error = now;
        LOGf("CAS: [%s] %s\n", r->channel->id, message);
    }
}

static void pending_clear(CA_PENDING *p) {
    for (int i = 0; i < 256; i++) free(p->data[i]);
    memset(p, 0, sizeof(*p));
}

void ca_input_init(INPUT *r) {
    r->cas = calloc(1, sizeof(*r->cas));
    if (!r->cas) { LOG("CAS: Out of memory.\n"); exit(1); }
    pthread_mutex_init(&r->cas->lock, NULL);
}

void ca_input_reset(INPUT *r) {
    struct ca_input *s = r->cas;
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->published_size = 0;
    s->scrambled = 0;
    s->emm_head = s->emm_count = 0; s->emm_received_ms = 0;
    s->shared_output_pid = 0; s->shared_accepted = s->shared_conflict = 0;
    pthread_mutex_unlock(&s->lock);
    pending_clear(&s->pending);
    memset(&s->pmt_assembler, 0, sizeof(s->pmt_assembler));
    memset(&s->cat_assembler, 0, sizeof(s->cat_assembler));
    memset(&s->emm_assembler, 0, sizeof(s->emm_assembler)); s->emm_assembler.private_emm = 1;
    s->shared_input_pid = 0;
    memset(s->map, 0, sizeof(s->map));
    s->pmt_size = s->cat_size = 0;
    s->have_cat = 0;
    s->deferred_pmt_size = s->deferred_cat_size = 0; s->have_deferred_cat = 0;
    /* Slots and output PMT version survive reconnect: old queued packets can
     * never acquire the identity of a different incoming CA PID. */
}

void ca_input_free(INPUT *r) {
    if (!r->cas) return;
    ca_input_reset(r);
    pthread_mutex_destroy(&r->cas->lock);
    free(r->cas); r->cas = NULL;
}

uint16_t ca_packet_pid(INPUT *r, uint16_t pid) {
    return r->cas && pid < 8192 ? r->cas->map[pid] : 0;
}

typedef void (*section_fn)(INPUT *, const uint8_t *, size_t);

static void consume(CA_ASSEMBLER *s, INPUT *r, const uint8_t *data,
                    size_t len, int can_start, section_fn emit) {
    while (len) {
        if (!s->used && (!can_start || *data == 0xff)) return;
        size_t target = s->used < 3 ? 3 : s->needed;
        size_t take = target - s->used;
        if (take > len) take = len;
        memcpy(s->data + s->used, data, take);
        s->used += take; data += take; len -= take;
        if (s->used == 3) {
            s->needed = 3 + (get16(s->data + 1) & 4095);
            if (s->needed < (s->private_emm ? 3 : 12) ||
                s->needed > (s->private_emm ? EMM_MAX : CA_SECTION_MAX)) {
                s->used = s->needed = 0;
                return;
            }
        }
        if (s->needed && s->used == s->needed) {
            if ((s->private_emm && s->data[0] >= 0x82 && s->data[0] <= 0x8f) ||
                (!s->private_emm && (s->data[1] & 0x80) && (s->data[5] & 1) &&
                 eit_crc32(s->data, s->needed) == 0))
                emit(r, s->data, s->needed);
            s->used = s->needed = 0;
            if (!can_start) return;
        }
    }
}

static void assemble(CA_ASSEMBLER *s, INPUT *r, const uint8_t p[188], section_fn emit) {
    if (p[0] != 0x47 || (p[1] & 0x80) || (p[3] & 0xc0) || !(p[3] & 0x30)) {
        s->used = s->needed = 0; s->cc_valid = 0; return;
    }
    size_t off = 4;
    if (p[3] & 0x20) {
        off += 1 + p[4];
        if (off > 188) { s->used = s->needed = 0; s->cc_valid = 0; return; }
        if (p[4] && (p[5] & 0x80)) { s->used = s->needed = 0; s->cc_valid = 0; }
    }
    if (!(p[3] & 0x10) || off == 188) return;
    int cc = p[3] & 15;
    if (s->cc_valid) {
        if (cc == s->cc && !memcmp(s->last_packet, p, 188)) return;
        if (cc != ((s->cc + 1) & 15)) s->used = s->needed = 0;
    }
    s->cc_valid = 1; s->cc = cc;
    memcpy(s->last_packet, p, 188);
    if (p[1] & 0x40) {
        unsigned pointer = p[off++];
        if (pointer > 188 - off) { s->used = s->needed = 0; return; }
        if (s->used && pointer) consume(s, r, p + off, pointer, 0, emit);
        s->used = s->needed = 0;
        off += pointer;
        consume(s, r, p + off, 188 - off, 1, emit);
    } else if (s->used) consume(s, r, p + off, 188 - off, 0, emit);
}

/* All descriptor loops are checked before accessing CA fields or handing the
 * PMT to the old libtsfuncs parser (whose own bounds checks are insufficient). */
static int descriptors_valid(const uint8_t *p, size_t len) {
    while (len) {
        if (len < 2 || (size_t)p[1] + 2 > len) return 0;
        if (p[0] == 9 && (p[1] < 4 || get_ts_pid(p + 4) < 32 || get_ts_pid(p + 4) == 8191))
            return 0;
        size_t size = p[1] + 2;
        p += size; len -= size;
    }
    return 1;
}

static int pmt_valid(INPUT *r, const uint8_t *p, size_t len) {
    if (len < 16 || p[0] != 2 || p[6] || p[7] || get16(p + 3) != r->stream.input_service_id)
        return 0;
    unsigned pcr = get_ts_pid(p + 8);
    if (pcr < 32 || pcr == 8191 || pcr == r->stream.pmt_pid) return 0;
    size_t info = get16(p + 10) & 4095;
    if (info > len - 16 || !descriptors_valid(p + 12, info)) return 0;
    size_t off = 12 + info;
    while (off < len - 4) {
        if (len - 4 - off < 5) return 0;
        unsigned pid = get_ts_pid(p + off + 1);
        size_t size = get16(p + off + 3) & 4095;
        if (pid < 32 || pid == 8191 || pid == r->stream.pmt_pid ||
            size > len - 4 - off - 5 || !descriptors_valid(p + off + 5, size)) return 0;
        off += 5 + size;
    }
    return off == len - 4;
}

static size_t packetize(const uint8_t *data, size_t len, unsigned pid, uint8_t cc,
                        uint8_t *packets) {
    size_t offset = 0, count = 0;
    while (offset < len) {
        uint8_t *p = packets + count * 188;
        memset(p, 0xff, 188);
        p[0] = 0x47; p[1] = (pid >> 8) | (!count ? 0x40 : 0); p[2] = pid;
        p[3] = 0x10 | ((cc + count) & 15);
        size_t start = count ? 4 : 5;
        if (!count) p[4] = 0;
        size_t take = len - offset;
        if (take > 188 - start) take = 188 - start;
        memcpy(p + start, data + offset, take);
        offset += take; count++;
    }
    return count;
}

static int ordinary_add(PIDREF *ref, unsigned pid, unsigned end) {
    if (pidref_get_new_pid(ref, pid)) return 1;
    if (ref->base_pid >= end) return 0;
    if (!pidref_add(ref, pid, ref->base_pid)) return 0;
    ref->base_pid++;
    return 1;
}

static int ca_descriptors_map(INPUT *r, uint8_t *p, size_t len, PIDREF *ordinary,
                              uint16_t slots[CA_SLOTS], uint16_t map[8192], int *count, int cat) {
    while (len) {
        size_t size = p[1] + 2;
        if (p[0] == 9) {
            unsigned pid = get_ts_pid(p + 4);
            if (pid == r->stream.pmt_pid || pid == r->stream.nit_pid ||
                pidref_get_new_pid(ordinary, pid)) return 0;
            if (r->channel->emm_shared_output_pid && pid == r->channel->emm_shared_pid) {
                /* Never alias an ECM, foreign CA descriptor or ordinary PID. */
                if (!cat || get16(p + 2) != r->channel->emm_shared_caid) return 0;
                map[pid] = r->channel->emm_shared_output_pid;
                setpid(p + 4, map[pid]); (*count)++;
                p += size; len -= size; continue;
            }
            unsigned slot = 0;
            for (; slot < CA_SLOTS && slots[slot] != pid; slot++);
            if (slot == CA_SLOTS) {
                for (slot = 0; slot < CA_SLOTS && slots[slot]; slot++);
                if (slot == CA_SLOTS) return 0;
                slots[slot] = pid;
            }
            map[pid] = CA_BASE + (r->channel->index - 1) * CA_SLOTS + slot;
            setpid(p + 4, map[pid]);
            (*count)++;
        }
        p += size; len -= size;
    }
    return 1;
}

static void crc_update(uint8_t *data, size_t len) {
    uint32_t crc = eit_crc32(data, len - 4);
    for (int i = 0; i < 4; i++) data[len - 4 + i] = crc >> (24 - i * 8);
}

/* Transaction: every capacity/conflict check occurs against local candidate
 * maps. Failed tables do not consume sticky slots or partially publish CAT. */
static int rebuild(INPUT *r, const uint8_t *pmt, size_t pmt_size,
                   const uint8_t *cat, size_t cat_size) {
    struct ca_input *s = r->cas;
    if (r->channel->index < 1 || r->channel->index > 31 || cat_size > CA_CAT_MAX) return 0;
    if (!pmt_size) { memmove(s->cat, cat, cat_size); s->cat_size = cat_size; return 1; }
    int cat_changed = s->cat_size != cat_size || memcmp(s->cat, cat, cat_size);
    uint16_t slots[CA_SLOTS], map[8192] = {0};
    memcpy(slots, s->slots, sizeof(slots));
    uint8_t rewritten[CA_SECTION_MAX], cat_out[CA_CAT_MAX];
    memcpy(rewritten, pmt, pmt_size); memcpy(cat_out, cat, cat_size);
    PIDREF *ordinary = pidref_init(32, r->channel->base_pid);
    unsigned end = r->channel->base_pid + 32;
    unsigned pcr = get_ts_pid(pmt + 8);
    int ca_count = 0, emm_count = 0;
    if (!ordinary_add(ordinary, r->stream.pmt_pid, end) || !ordinary_add(ordinary, pcr, end)) goto invalid;
    size_t info = get16(pmt + 10) & 4095;
    size_t off = 12 + info;
    for (; off < pmt_size - 4;) {
        unsigned pid = get_ts_pid(pmt + off + 1);
        if (!ordinary_add(ordinary, pid, end)) goto invalid;
        off += 5 + (get16(pmt + off + 3) & 4095);
    }
    if (!ca_descriptors_map(r, rewritten + 12, info, ordinary, slots, map, &ca_count, 0)) goto invalid;
    for (off = 12 + info; off < pmt_size - 4;) {
        size_t size = get16(pmt + off + 3) & 4095;
        if (!ca_descriptors_map(r, rewritten + off + 5, size, ordinary, slots, map, &ca_count, 0)) goto invalid;
        setpid(rewritten + off + 1, pidref_get_new_pid(ordinary, get_ts_pid(pmt + off + 1)));
        off += 5 + size;
    }
    if (!ca_descriptors_map(r, cat_out, cat_size, ordinary, slots, map, &emm_count, 1)) goto invalid;
    set16(rewritten + 3, r->channel->service_id);
    setpid(rewritten + 8, pidref_get_new_pid(ordinary, pcr));
    int version = s->have_version ? (s->pmt_version + 1) & 31 :
        (config->table_version + ((pmt[5] >> 1) & 31) + 1) & 31;
    rewritten[5] = (rewritten[5] & 0xc1) | (version << 1);
    crc_update(rewritten, pmt_size);
    uint8_t packets[6 * 188];
    size_t count = packetize(rewritten, pmt_size, r->channel->pmt_pid, 0, packets);
    struct ts_pmt *new_pmt = ts_pmt_alloc();
    for (size_t i = 0; i < count; i++) ts_pmt_push_packet(new_pmt, packets + i * 188);
    if (!new_pmt->initialized) { ts_pmt_free(&new_pmt); goto invalid; }

    /* A CAT-only update does not change PMT PID assignments or content. Keep
     * its already-published version and packet template in that case. */
    if (s->pmt_size == pmt_size && !memcmp(s->pmt, pmt, pmt_size) && r->stream.pmt_rewritten) {
        ts_pmt_free(&new_pmt);
    } else {
        ts_pmt_free(&r->stream.pmt_rewritten);
        r->stream.pmt_rewritten = new_pmt;
        s->pmt_version = version; s->have_version = 1;
    }
    pidref_free(&r->stream.pidref); r->stream.pidref = ordinary;
    r->stream.pcr_pid = pcr;
    r->output_pcr_pid = pidref_get_new_pid(ordinary, pcr);
    memmove(s->pmt, pmt, pmt_size); s->pmt_size = pmt_size;
    memmove(s->cat, cat, cat_size); s->cat_size = cat_size;
    memcpy(s->slots, slots, sizeof(slots)); memcpy(s->map, map, sizeof(map));
    uint16_t shared = r->channel->emm_shared_pid;
    if (!shared || map[shared] != r->channel->emm_shared_output_pid) shared = 0;
    int shared_changed = shared != s->shared_input_pid || cat_changed;
    s->shared_input_pid = shared;
    if (shared_changed) {
        memset(&s->emm_assembler, 0, sizeof(s->emm_assembler)); s->emm_assembler.private_emm = 1;
    }
    pthread_mutex_lock(&s->lock);
    if (shared_changed) { s->emm_head = s->emm_count = 0; s->emm_received_ms = 0; }
    s->shared_output_pid = shared ? r->channel->emm_shared_output_pid : 0;
    s->shared_accepted = 0;
    memcpy(s->published_cat, cat_out, cat_size); s->published_size = cat_size;
    s->scrambled = ca_count != 0;
    pthread_mutex_unlock(&s->lock);
    return 1;
invalid:
    pidref_free(&ordinary);
    return 0;
}

static void publish_cat_metadata(struct ca_input *s, int version, int last, const uint32_t crc[256]) {
    s->have_cat = 1; s->cat_version = version; s->cat_last = last;
    memcpy(s->cat_crc, crc, sizeof(s->cat_crc));
}

static void rejected(INPUT *r) {
    error(r, "CAS: Table deferred/rejected (PID conflict, ordinary 32-PID block or 128 lifetime CA slots exhausted).");
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void emit_pmt(INPUT *r) {
    struct ts_pmt *pmt = r->stream.pmt_rewritten;
    if (!pmt) return;
    for (int i = 0; i < pmt->section_header->num_packets; i++)
        ts_packet_set_cont(pmt->section_header->packet_data + i * 188, r->stream.pid_pmt_cont + i);
    r->stream.pid_pmt_cont += pmt->section_header->num_packets;
    input_buffer_add(r, pmt->section_header->packet_data, pmt->section_header->num_packets * 188);
    r->cas->pmt_emitted_ms = monotonic_ms();
}

static void pmt_section(INPUT *r, const uint8_t *data, size_t len) {
    struct ca_input *s = r->cas;
    if (!pmt_valid(r, data, len)) { error(r, "CAS: Invalid PMT rejected."); return; }
    uint64_t now = monotonic_ms();
    if (now - s->deferred_pmt_ms > 10000) s->deferred_pmt_size = 0;
    if (now - s->deferred_cat_ms > 10000) s->have_deferred_cat = 0;
    if (s->pmt_size == len && !memcmp(s->pmt, data, len)) { emit_pmt(r); return; }
    /* A valid transition may exchange an ES PID with an EMM PID. Neither new
     * table then fits its old counterpart. Retain at most one validated PMT
     * and one complete CAT so the compatible pair can commit atomically. */
    int committed = rebuild(r, data, len, s->cat, s->cat_size);
    if (!committed && s->have_deferred_cat && rebuild(r, data, len, s->deferred_cat, s->deferred_cat_size)) {
        publish_cat_metadata(s, s->deferred_cat_version, s->deferred_cat_last, s->deferred_cat_crc);
        s->have_deferred_cat = 0; s->deferred_cat_size = 0;
        committed = 1;
    }
    if (committed) {
        s->deferred_pmt_size = 0;
        emit_pmt(r);
    } else {
        memcpy(s->deferred_pmt, data, len); s->deferred_pmt_size = len; s->deferred_pmt_ms = now;
        rejected(r);
    }
}

void ca_input_tick(INPUT *r) {
    if (!r->cas || !r->cas->pmt_size || !r->stream.pmt_rewritten) return;
    unsigned interval = config->timeouts.pmt < 50 ? 50 : config->timeouts.pmt;
    if (monotonic_ms() - r->cas->pmt_emitted_ms >= interval) emit_pmt(r);
}

void ca_pmt_packet(INPUT *r, const uint8_t packet[188]) {
    assemble(&r->cas->pmt_assembler, r, packet, pmt_section);
}

static void cat_section(INPUT *r, const uint8_t *data, size_t len) {
    struct ca_input *s = r->cas;
    CA_PENDING *pending = &s->pending;
    if (data[0] != 1 || data[6] > data[7] || !descriptors_valid(data + 8, len - 12)) {
        error(r, "CAS: Invalid CAT rejected."); return;
    }
    int version = (data[5] >> 1) & 31, section = data[6], last = data[7];
    uint32_t crc = ((uint32_t)get16(data + len - 4) << 16) | get16(data + len - 2);
    /* Repeated sections of the committed table need no allocation or rebuild. */
    if (!pending->valid && s->have_cat && version == s->cat_version && last == s->cat_last && s->cat_crc[section] == crc)
        return;
    if (!pending->valid || pending->version != version || pending->last != last) {
        pending_clear(pending);
        pending->valid = 1; pending->version = version; pending->last = last;
    }
    size_t bytes = pending->bytes - pending->size[section] + len;
    if (bytes > CA_CAT_MAX + 12 * (size_t)(last + 1)) {
        pending_clear(pending); error(r, "CAS: CAT exceeds 4096 descriptor bytes; table rejected."); return;
    }
    uint8_t *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    free(pending->data[section]); pending->data[section] = copy;
    pending->size[section] = len; pending->bytes = bytes;
    for (int i = 0; i <= last; i++) if (!pending->data[i]) return;

    /* Keep only standard CA descriptors. Non-CA/private descriptor scope from
     * unrelated input CATs must not accidentally carry into another source. */
    uint8_t cat[CA_CAT_MAX]; size_t size = 0;
    for (int i = 0; i <= last; i++) {
        const uint8_t *p = pending->data[i] + 8;
        size_t left = pending->size[i] - 12;
        while (left) {
            size_t take = p[1] + 2;
            if (p[0] == 9) {
                if (size + take > sizeof(cat)) { pending_clear(pending); return; }
                memcpy(cat + size, p, take); size += take;
            }
            p += take; left -= take;
        }
    }
    uint32_t crcs[256] = {0};
    for (int i = 0; i <= last; i++) {
        const uint8_t *tail = pending->data[i] + pending->size[i] - 4;
        crcs[i] = ((uint32_t)get16(tail) << 16) | get16(tail + 2);
    }
    uint64_t now = monotonic_ms();
    if (now - s->deferred_pmt_ms > 10000) s->deferred_pmt_size = 0;
    if (now - s->deferred_cat_ms > 10000) s->have_deferred_cat = 0;
    int committed = rebuild(r, s->pmt, s->pmt_size, cat, size);
    if (!committed && s->deferred_pmt_size && rebuild(r, s->deferred_pmt, s->deferred_pmt_size, cat, size)) {
        s->deferred_pmt_size = 0;
        emit_pmt(r);
        committed = 1;
    }
    if (committed) {
        publish_cat_metadata(s, version, last, crcs);
        s->have_deferred_cat = 0; s->deferred_cat_size = 0;
    } else {
        memcpy(s->deferred_cat, cat, size); s->deferred_cat_size = size;
        s->have_deferred_cat = 1; s->deferred_cat_ms = now; s->deferred_cat_version = version; s->deferred_cat_last = last;
        memcpy(s->deferred_cat_crc, crcs, sizeof(crcs));
        rejected(r);
    }
    pending_clear(pending);
}

void ca_cat_packet(INPUT *r, const uint8_t packet[188]) {
    assemble(&r->cas->cat_assembler, r, packet, cat_section);
}

static void shared_emm_section(INPUT *r, const uint8_t *data, size_t len) {
    struct ca_input *s = r->cas;
    pthread_mutex_lock(&s->lock);
    s->emm_received_ms = monotonic_ms();
    if (s->emm_count < EMM_QUEUE) {
        EMM_SECTION *section = &s->emms[(s->emm_head + s->emm_count) % EMM_QUEUE];
        section->size = len; memcpy(section->data, data, len); s->emm_count++;
    }
    pthread_mutex_unlock(&s->lock);
}

int ca_shared_emm_packet(INPUT *r, uint16_t pid, const uint8_t packet[188]) {
    if (!r->cas || !r->cas->shared_input_pid || pid != r->cas->shared_input_pid) return 0;
    if (config->emm_shared_id[0] && r->channel->emm_shared_output_pid == SHARED_BASE) return 1;
    assemble(&r->cas->emm_assembler, r, packet, shared_emm_section);
    return 1;
}

static void output_snapshot(OUTPUT *o) {
    struct ca_output *s = o->cas;
    uint8_t descriptors[CA_MERGED_MAX]; size_t size = 0;
    uint8_t shared_desc[SHARED_GROUPS][257] = {{0}};
    unsigned shared_size[SHARED_GROUPS] = {0};
    if (config->emm_shared_id[0]) {
        uint8_t d[6] = {9, 4, 0, 0, 0xe0, 0};
        set16(d + 2, config->emm_shared_caid); setpid(d + 4, SHARED_BASE);
        memcpy(shared_desc[0], d, sizeof(d)); shared_size[0] = sizeof(d);
        memcpy(descriptors, d, sizeof(d)); size = sizeof(d);
    }
    int encrypted[32] = {0};
    LNODE *node, *tmp;
    list_lock(config->inputs);
    list_for_each(config->inputs, node, tmp) {
        INPUT *r = node->data;
        if (!r->cas || r->channel->index < 1 || r->channel->index > 31) continue;
        pthread_mutex_lock(&r->cas->lock);
        struct ca_input *in = r->cas;
        in->shared_accepted = in->shared_output_pid != 0 &&
            !(config->emm_shared_id[0] && in->shared_output_pid == SHARED_BASE);
        int conflict = 0;
        for (size_t off = 0; off < in->published_size;) {
            const uint8_t *d = in->published_cat + off;
            size_t take = d[1] + 2;
            unsigned pid = get_ts_pid(d + 4);
            int duplicate = 0;
            if (pid >= SHARED_BASE && pid < SHARED_BASE + SHARED_GROUPS) {
                unsigned group = pid - SHARED_BASE;
                if (!shared_size[group]) {
                    memcpy(shared_desc[group], d, take); shared_size[group] = take;
                } else {
                    duplicate = 1;
                    if (shared_size[group] != take || memcmp(shared_desc[group], d, take)) {
                        conflict = 1; in->shared_accepted = 0;
                    }
                }
            }
            if (!duplicate && size + take <= sizeof(descriptors)) {
                memcpy(descriptors + size, d, take); size += take;
            }
            off += take;
        }
        if (conflict && !in->shared_conflict)
            LOGf("CAS: [%s] Shared EMM descriptor conflict; this input's EMM is withheld.\n", r->channel->id);
        in->shared_conflict = conflict;
        encrypted[r->channel->index] = r->cas->scrambled;
        pthread_mutex_unlock(&r->cas->lock);
    }
    list_unlock(config->inputs);
    s->shared_present = 0;
    for (unsigned group = config->emm_shared_id[0] ? 1 : 0; group < SHARED_GROUPS; group++)
        if (shared_size[group]) s->shared_present = 1;
    if (!s->initialized || s->size != size || memcmp(s->descriptors, descriptors, size)) {
        memcpy(s->descriptors, descriptors, size); s->size = size;
        s->count = 1; s->sections[0] = (CA_SPAN){0, 0};
        for (size_t off = 0; off < size;) {
            size_t take = descriptors[off + 1] + 2;
            CA_SPAN *span = &s->sections[s->count - 1];
            if (span->size + take > 1012) {
                /* 31 bounded CATs fit in <=170 sections even at worst packing. */
                if (s->count == CA_CAT_SECTIONS) { LOG("CAS: Internal CAT section overflow.\n"); exit(1); }
                span = &s->sections[s->count++]; *span = (CA_SPAN){off, 0};
            }
            span->size += take; off += take;
        }
        s->version = s->initialized ? (s->version + 1) & 31 : (config->table_version + 1) & 31;
        s->initialized = 1; s->cursor = 0;
    }
    int changed = 0;
    list_lock(config->channels);
    list_for_each(config->channels, node, tmp) {
        CHANNEL *channel = node->data;
        if (channel->index < 1 || channel->index > 31) continue;
        for (int i = 0; i < o->sdt->streams_num; i++) {
            struct ts_sdt_stream *stream = o->sdt->streams[i];
            if (stream->service_id == channel->service_id && stream->free_CA_mode != encrypted[channel->index]) {
                stream->free_CA_mode = encrypted[channel->index]; changed = 1;
            }
        }
    }
    list_unlock(config->channels);
    if (changed) {
        o->sdt->section_header->version_number = (o->sdt->section_header->version_number + 1) & 31;
        ts_sdt_regenerate_packet_data(o->sdt);
    }
}

static void shared_output_tick(OUTPUT *o) {
    struct ca_output *out = o->cas;
    if (!out->shared_present) return;
    INPUT *chosen[SHARED_GROUPS] = {0};
    uint64_t now = monotonic_ms();
    LNODE *node, *tmp;
    list_lock(config->inputs);
    /* Prefer the current source during a burst. A fresh complete section from
     * another source takes over when the old source misses the next burst.
     * Idle carousels do not generate traffic or repeat cached authorizations. */
    list_for_each(config->inputs, node, tmp) {
        INPUT *r = node->data;
        struct ca_input *in = r->cas;
        if (!in) continue;
        pthread_mutex_lock(&in->lock);
        unsigned pid = in->shared_output_pid;
        if (in->shared_accepted && pid >= SHARED_BASE && pid < SHARED_BASE + SHARED_GROUPS &&
            in->emm_received_ms && now - in->emm_received_ms <= 2000) {
            unsigned group = pid - SHARED_BASE;
            if (out->emm_owner[group] == (unsigned)r->channel->index)
                chosen[group] = r;
            else if (!chosen[group] && in->emm_count)
                chosen[group] = r;
        }
        pthread_mutex_unlock(&in->lock);
    }
    unsigned budget = 32;
    list_for_each(config->inputs, node, tmp) {
        INPUT *r = node->data;
        struct ca_input *in = r->cas;
        if (!in) continue;
        pthread_mutex_lock(&in->lock);
        unsigned pid = in->shared_output_pid;
        unsigned group = pid >= SHARED_BASE ? pid - SHARED_BASE : SHARED_GROUPS;
        if (group >= SHARED_GROUPS || chosen[group] != r || !in->shared_accepted) {
            in->emm_head = in->emm_count = 0;
        } else {
            out->emm_owner[group] = r->channel->index;
            while (in->emm_count && budget) {
                EMM_SECTION *section = &in->emms[in->emm_head];
                uint8_t packets[23 * 188];
                size_t count = packetize(section->data, section->size, pid, out->emm_cc[group], packets);
                if (cbuf_fill(o->psibuf, packets, count * 188) != 0) break;
                out->emm_cc[group] = (out->emm_cc[group] + count) & 15;
                in->emm_head = (in->emm_head + 1) % EMM_QUEUE; in->emm_count--; budget--;
            }
        }
        pthread_mutex_unlock(&in->lock);
    }
    list_unlock(config->inputs);
}

static void global_emm_tick(OUTPUT *o) {
    struct ca_output *s = o->cas;
    if (!config->emm_shared_id[0]) return;
    uint64_t now = monotonic_ms();
    if (s->emm_socket < 0) {
        if (now < s->emm_retry_ms) return;
        s->emm_retry_ms = now + 1000;
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        struct sockaddr_in address = {.sin_family = AF_INET,
            .sin_port = htons(config->emm_shared_port), .sin_addr = config->emm_shared_group};
        struct ip_mreq membership = {.imr_multiaddr = config->emm_shared_group,
                                    .imr_interface = config->emm_shared_interface};
        if (fd < 0) goto socket_failed;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) goto socket_failed;
#ifdef IP_MULTICAST_ALL
        int zero = 0;
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero)) < 0) goto socket_failed;
#endif
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) goto socket_failed;
        s->emm_socket = fd;
        goto socket_ready;
socket_failed:
        if (fd >= 0) close(fd);
        if (time(NULL) != s->emm_last_error) {
            s->emm_last_error = time(NULL);
            LOG("CAS: Shared EMM multicast listener unavailable; retrying.\n");
        }
        return;
    }
socket_ready:
    /* Live sections only: no cache replay on loss. Limit both bytes and work
     * per tick; one bad/truncated datagram never contributes a partial EMM. */
    unsigned budget = 32;
    for (unsigned burst = 0; burst < 32 && budget; burst++) {
        uint8_t data[8193];
        ssize_t length = recv(s->emm_socket, data, sizeof(data), 0);
        if (length < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(s->emm_socket); s->emm_socket = -1;
            }
            break;
        }
        if (!length || length > 8192) continue;
        size_t off = 0; unsigned count = 0;
        while (off < (size_t)length) {
            if ((size_t)length - off < 3 || data[off] < 0x82 || data[off] > 0x8f) break;
            size_t size = 3 + (get16(data + off + 1) & 4095);
            if (size > EMM_MAX || size > (size_t)length - off || ++count > budget) break;
            off += size;
        }
        if (off != (size_t)length) continue;
        for (off = 0; off < (size_t)length;) {
            size_t size = 3 + (get16(data + off + 1) & 4095);
            uint8_t packets[23 * 188];
            size_t packet_count = packetize(data + off, size, SHARED_BASE, s->emm_cc[0], packets);
            if (cbuf_fill(o->psibuf, packets, packet_count * 188) == 0)
                s->emm_cc[0] = (s->emm_cc[0] + packet_count) & 15;
            off += size; budget--;
        }
    }
}

void ca_output_tick(OUTPUT *o, struct timeval *now) {
    if (!o->cas) {
        o->cas = calloc(1, sizeof(*o->cas));
        if (!o->cas) { LOG("CAS: Out of memory.\n"); exit(1); }
        o->cas->emm_socket = -1;
    }
    struct ca_output *s = o->cas;
    if (!s->initialized || (s->cursor == s->count && timeval_diff_msec(&s->refresh, now) >= 500)) {
        s->refresh = *now; output_snapshot(o);
    }
    if (s->cursor == s->count && timeval_diff_msec(&s->cycle, now) >= 500) s->cursor = 0;
    if (!s->cursor) s->cycle = *now;
    /* Bound each PSI-thread burst; large merged CATs never monopolize psibuf. */
    for (int burst = 0; burst < 4 && s->cursor < s->count; burst++) {
        CA_SPAN *span = &s->sections[s->cursor];
        uint8_t section[1024] = {1, 0xb0, 0, 0xff, 0xff, 0xc1, 0, 0};
        size_t len = span->size + 12;
        set16(section + 1, 0xb000 | (len - 3));
        section[5] |= s->version << 1; section[6] = s->cursor; section[7] = s->count - 1;
        memcpy(section + 8, s->descriptors + span->start, span->size);
        crc_update(section, len);
        uint8_t packets[6 * 188];
        size_t count = packetize(section, len, 1, s->cc, packets);
        if (cbuf_fill(o->psibuf, packets, count * 188) != 0) break;
        s->cc = (s->cc + count) & 15; s->cursor++;
    }
    shared_output_tick(o);
    global_emm_tick(o);
}

void ca_output_free(OUTPUT *o) {
    if (o->cas && o->cas->emm_socket >= 0) close(o->cas->emm_socket);
    free(o->cas); o->cas = NULL;
}
