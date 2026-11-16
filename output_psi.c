/* smdbg fork modifications, 2026-11-16: see CHANGELOG.md. */
/*
 * mptsd output PSI handling
 * Copyright (C) 2010-2011 Unix Solutions Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 */
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "libfuncs/log.h"
#include "libfuncs/list.h"

#include "libtsfuncs/tsfuncs.h"

#include "config.h"
#include "data.h"

static void output_psi_init_pat(CONFIG *conf, OUTPUT *o) {
	LNODE *lc, *lctmp;
	o->pat = ts_pat_alloc_init(conf->transport_stream_id);
	list_lock(conf->channels);
	list_for_each(conf->channels, lc, lctmp) {
		CHANNEL *c = lc->data;
		ts_pat_add_program(o->pat, c->service_id, c->pmt_pid);
	}
	list_unlock(conf->channels);
	o->pat->section_header->version_number = conf->table_version;
	ts_pat_regenerate_packets(o->pat);
	gettimeofday(&o->pat_ts, NULL);
}

static void output_psi_init_nit(CONFIG *conf, OUTPUT *o) {
    struct ts_nit *nit = ts_nit_alloc_init(conf->network_id);
    if (!ts_nit_add_network_name_descriptor(nit, conf->network_name)) {
        LOG("CONFIG: Invalid NIT network name.\n"); exit(1);
    }
    LNODE *lc, *lctmp;
    list_lock(conf->nit);
    list_for_each(conf->nit, lc, lctmp) {
        NIT *n = lc->data;
        /* Backwards compatibility for manually maintained old config files. */
        if (!n->services_defined && n->ts_id == conf->transport_stream_id) {
            LNODE *cc, *ctmp;
            list_for_each(conf->channels, cc, ctmp) {
                CHANNEL *c = cc->data;
                int j = n->services_count++;
                n->services[j] = (c->service_id << 8) | (c->radio ? 2 : 1);
                n->lcns[j] = ((uint32_t)c->service_id << 16) | (c->lcn_visible << 15) | 0x7c00 | c->lcn;
            }
        }
        if (!ts_nit_add_stream_descriptors(nit, n->ts_id, conf->original_network_id,
            n->_freq, n->_modulation, n->_symbol_rate,
            conf->use_lcn ? n->lcns : NULL, n->services, n->services_count)) {
            LOG("CONFIG: NIT exceeds DVB section capacity; reduce network size.\n"); exit(1);
        }
    }
    list_unlock(conf->nit);
    nit->section_header->version_number = conf->table_version;
    ts_nit_regenerate_packet_data(nit);
    gettimeofday(&o->nit_ts, NULL);
    o->nit = nit;
}

static void output_psi_init_sdt(CONFIG *conf, OUTPUT *o) {
	LNODE *lc, *lctmp;
	struct ts_sdt *sdt = ts_sdt_alloc_init(conf->original_network_id, conf->transport_stream_id);
	list_lock(conf->channels);
	list_for_each(conf->channels, lc, lctmp) {
		CHANNEL *c = lc->data;
		if (!ts_sdt_add_service_descriptor(sdt, c->service_id, c->radio == 0, conf->provider_name, c->name)) {
			LOG("CONFIG: SDT exceeds DVB section or text capacity.\n"); exit(1);
		}
		sdt->streams[sdt->streams_num - 1]->EIT_schedule_flag = c->eit_mode == 1;
	}
	list_unlock(conf->channels);
	sdt->section_header->version_number = conf->table_version;
	ts_sdt_regenerate_packet_data(sdt);
	gettimeofday(&o->sdt_ts, NULL);
	o->sdt = sdt;
}

static void output_psi_init_tdt_tot(CONFIG *conf, OUTPUT *o) {
	(void)conf; // Silence warning
	o->pid_tdt_cont = 15;
	o->tdt = ts_tdt_alloc_init(time(NULL));
	o->tot = ts_tot_alloc_init(time(NULL));
	gettimeofday(&o->tdt_ts, NULL);
	gettimeofday(&o->tot_ts, NULL);
}


static void output_add_pat(OUTPUT *o) {
	if (!o->pat->programs_num) {
		LOG("OUTPUT: Error no programs in PAT!\n");
		return;
	}
	int i;
	struct ts_pat *pat = o->pat;
//	LOGf("OUTPUT: Outputing PAT with %d programs\n", o->pat->programs_num);
	for (i=0;i<pat->section_header->num_packets;i++) {
		ts_packet_set_cont(pat->section_header->packet_data + (i * TS_PACKET_SIZE), i + o->pid_pat_cont);
	}
	pat->ts_header.continuity = o->pid_pat_cont;
	o->pid_pat_cont += pat->section_header->num_packets;
	cbuf_fill(o->psibuf, pat->section_header->packet_data, pat->section_header->num_packets * TS_PACKET_SIZE);
//	ts_pat_dump(o->pat);
}

void output_add_nit(OUTPUT *o) {
	if (!o || !o->nit)
		return;
	int i;
	struct ts_nit *nit = o->nit;
//	LOGf("OUTPUT: Outputing NIT\n");
	for (i=0;i<nit->section_header->num_packets;i++) {
		ts_packet_set_cont(nit->section_header->packet_data + (i * TS_PACKET_SIZE), i + o->pid_nit_cont);
	}
	nit->ts_header.continuity = o->pid_nit_cont;
	o->pid_nit_cont += nit->section_header->num_packets;
	cbuf_fill(o->psibuf, nit->section_header->packet_data, nit->section_header->num_packets * TS_PACKET_SIZE);
//	ts_nit_dump(nit);
}

void output_add_sdt(OUTPUT *o) {
	if (!o || !o->sdt)
		return;
	int i;
	struct ts_sdt *sdt = o->sdt;
//	LOGf("OUTPUT: Outputing SDT\n");
	for (i=0;i<sdt->section_header->num_packets;i++) {
		ts_packet_set_cont(sdt->section_header->packet_data + (i * TS_PACKET_SIZE), i + o->pid_sdt_cont);
	}
	sdt->ts_header.continuity = o->pid_sdt_cont;
	o->pid_sdt_cont += sdt->section_header->num_packets;
	cbuf_fill(o->psibuf, sdt->section_header->packet_data, sdt->section_header->num_packets * TS_PACKET_SIZE);
//	ts_sdt_dump(o->sdt);
}

static void output_add_pid0x14(OUTPUT *o, struct ts_tdt *tdt) {
	if (!o || !o->tdt)
		return;
	int i;
//	LOGf("OUTPUT: Outputing TDT\n");
	for (i=0;i<tdt->section_header->num_packets;i++) {
		ts_packet_set_cont(tdt->section_header->packet_data + (i * TS_PACKET_SIZE), i + o->pid_tdt_cont);
	}
	tdt->ts_header.continuity = o->pid_tdt_cont;
	o->pid_tdt_cont += tdt->section_header->num_packets;
	cbuf_fill(o->psibuf, tdt->section_header->packet_data, tdt->section_header->num_packets * TS_PACKET_SIZE);
}

static void output_add_tdt(OUTPUT *o) {
//	LOGf("OUTPUT: Outputing TDT\n");
	ts_tdt_set_time(o->tdt, time(NULL));
	output_add_pid0x14(o, o->tdt);
//	ts_tdt_dump(o->tdt);
}

static void output_add_tot(OUTPUT *o) {
//	LOGf("OUTPUT: Outputing TOT\n");
	ts_tot_set_localtime_offset_sofia(o->tot, time(NULL));
	output_add_pid0x14(o, o->tot);
//	ts_tdt_dump(o->tot);
}

static void __output_add_eit(OUTPUT *o, struct ts_eit *eit) {
    if (eit && eit->section_header && eit->section_header->packet_data)
        cbuf_fill(o->psibuf, eit->section_header->packet_data,
                  eit->section_header->num_packets * TS_PACKET_SIZE);
}

static void output_add_eit(CONFIG *conf, OUTPUT *o) {
	LNODE *lr, *lrtmp;
	config_load_epg(conf);
	list_for_each(conf->inputs, lr, lrtmp) {
		INPUT *r = lr->data;
		if (r->channel->eit_mode == 1) continue;
		__output_add_eit(o, r->channel->eit_now);
		__output_add_eit(o, r->channel->eit_next);
	}
}

static void output_psi_add(CONFIG *conf, OUTPUT *o, struct timeval *now) {
	if (timeval_diff_msec(&o->pat_ts, now) >= conf->timeouts.pat) {
		o->pat_ts = *now;
		output_add_pat(o);
	}
	if (timeval_diff_msec(&o->nit_ts, now) >= conf->timeouts.nit) {
		o->nit_ts = *now;
		output_add_nit(o);
	}
	if (timeval_diff_msec(&o->sdt_ts, now) >= conf->timeouts.sdt) {
		o->sdt_ts = *now;
		output_add_sdt(o);
	}
	if (timeval_diff_msec(&o->tdt_ts, now) >= conf->timeouts.tdt) {
		o->tdt_ts = *now;
		output_add_tdt(o);
	}
	if (timeval_diff_msec(&o->tot_ts, now) >= conf->timeouts.tot) {
		o->tot_ts = *now;
		output_add_tot(o);
	}
	if (timeval_diff_msec(&o->eit_ts, now) >= conf->timeouts.eit) {
		o->eit_ts = *now;
		output_add_eit(conf, o);
	}
}


void output_psi_init(CONFIG *conf, OUTPUT *output) {
	output_psi_init_pat(conf, output);
	output_psi_init_nit(conf, output);
	output_psi_init_sdt(conf, output);
	output_psi_init_tdt_tot(conf, output);
	gettimeofday(&output->eit_ts, NULL);
}

void output_psi_free(OUTPUT *o) {
    ca_output_free(o);
	ts_pat_free(&o->pat);
	ts_nit_free(&o->nit);
	ts_sdt_free(&o->sdt);
	ts_tdt_free(&o->tdt);
	ts_tdt_free(&o->tot);
}

void * output_handle_psi(void *_config) {
	CONFIG *conf = _config;
	OUTPUT *o = conf->output;
	struct timeval now;

	signal(SIGPIPE, SIG_IGN);
	while (!o->dienow) {
		gettimeofday(&now, NULL);
		ca_output_tick(o, &now);
		output_psi_add(conf, o, &now);
		usleep(10000); // 10 ms
	}
	LOG("OUTPUT: PSI thread stopped.\n");
	o->dienow++;
	return 0;
}
