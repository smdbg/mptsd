#include <assert.h>
#include <string.h>
#include "libtsfuncs/tsfuncs.h"
int main(void) {
    char provider[] = "ДВБ", name[] = "БНТ";
    struct ts_sdt *sdt = ts_sdt_alloc_init(99, 5311);
    assert(ts_sdt_add_service_descriptor(sdt, 1001, 1, provider, name));
    uint8_t *d = sdt->streams[0]->descriptor_data;
    assert(d[3] == strlen(provider)+1 && d[4] == 0x15);
    assert(!memcmp(d+5, provider, strlen(provider)));
    unsigned pos = 5+strlen(provider);
    assert(d[pos] == strlen(name)+1 && d[pos+1] == 0x15);
    assert(!memcmp(d+pos+2, name, strlen(name)));
    char long_name[201]; memset(long_name, 'a', 200); long_name[200] = 0;
    int before = sdt->streams_num;
    while (ts_sdt_add_service_descriptor(sdt, 1200, 1, "DVB", long_name)) before++;
    assert(sdt->streams_num == before && sdt->section_header->section_length <= 1021);
    ts_sdt_free(&sdt);

    struct ts_nit *nit = ts_nit_alloc_init(1);
    assert(ts_nit_add_network_name_descriptor(nit, provider));
    assert(nit->network_info[2] == 0x15 && nit->network_info[1] == strlen(provider)+1);
    uint32_t services[31], lcns[31];
    for (unsigned i=0; i<31; i++) {
        services[i] = ((1001+i)<<8) | 1;
        lcns[i] = ((1001+i)<<16) | 0xfc00 | (i+1);
    }
    for (unsigned i=0; i<4; i++)
        assert(ts_nit_add_stream_descriptors(nit, 5311+i, 99, 0x05140000, 4, 0x0069000, lcns, services, 31));
    assert(!ts_nit_add_stream_descriptors(nit, 5315, 99, 0x05460000, 4, 0x0069000, lcns, services, 31));
    assert(nit->streams_num == 4 && nit->section_header->section_length <= 1021);
    ts_nit_free(&nit);
    return 0;
}
