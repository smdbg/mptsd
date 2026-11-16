/* smdbg fork modifications, 2026-11-16: see CHANGELOG.md. */
/*
 * mptsd input handling
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
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "libfuncs/io.h"
#include "libfuncs/log.h"

#include "libtsfuncs/tsfuncs.h"

#include "data.h"
#include "config.h"
#include "network.h"

extern int keep_going;
extern CONFIG *config;

// #define dump_tables 1

#define MAX_ZERO_READS 3

/*         Start: 3 seconds on connect */
/* In connection: Max UDP timeout == 3 seconds (read) + 2 seconds (connect) == 5 seconds */
#define UDP_READ_RETRIES 3
#define UDP_READ_TIMEOUT 1000

/*         Start: 1/4 seconds on connect */
/* In connection: Max TCP timeout == 5 seconds (read) + 2 seconds (connect)             == 7 seconds */
/* In connection: Max TCP timeout == 5 seconds (read) + 8 seconds (connect, host unrch) == 13 seconds */
#define TCP_READ_RETRIES 5
#define TCP_READ_TIMEOUT 1000

// Init pmt_pid and nit_pid
// Return 0 on error, 1 on success
int input_process_pat(INPUT *r) {
	int i;
	int num_programs = 0;
	INPUT_STREAM *s = &r->stream;
	struct ts_pat *pat = s->pat;

	s->nit_pid = 0x10; // Default NIT pid
	for (i=0;i<pat->programs_num;i++) {
		struct ts_pat_program *prg = pat->programs[i];
		if (prg->pid) {
			if (prg->program == 0) { // NIT
				s->nit_pid = prg->pid;
			} else { // PAT
				s->pmt_pid = prg->pid;
				s->input_service_id = prg->program;
				num_programs++;
				// Continue counting: multi-program input must not be silently misrouted.
			}
		}
	}

	// MPTS is not supported as input stream in the moment
	if (num_programs > 1) {
		LOGf("INPUT : %-10s | Can't handle MPTS (%d programs) as input stream\n", r->channel->id, num_programs);
		return 0;
	}

	return 1;
}

void input_rewrite_pat(INPUT *r) {
	int i;
	INPUT_STREAM *s = &r->stream;
	struct ts_pat *new_pat = ts_pat_copy(s->pat);
	if (!new_pat)
		return;

	// Rewrite PAT pids
	for (i=0;i<new_pat->programs_num;i++) {
		struct ts_pat_program *prg = new_pat->programs[i];
		if (prg->program != 0) { // Skip NIT
			// Add pid to rewriter
			pidref_add(s->pidref, prg->pid, s->pidref->base_pid);
			// Rewrite PAT
			prg->program = r->channel->service_id;
			prg->pid     = s->pidref->base_pid;
			s->pidref->base_pid++;
		}
	}

	// Save rewritten packet
	ts_pat_regenerate_packets(new_pat);
	s->pat_rewritten = new_pat;
}

void input_buffer_add(INPUT *r, uint8_t *data, int datasize) {
	if (r->dienow)
		return;
	if (r->ifd)
		if(write(r->ifd, data, datasize) < 0) {;}
	if (r->disabled) {
		unsigned long bufsize = r->buf->input - r->buf->output;
		double buffull = ((double)bufsize / r->buf->size) * 100;
		if (buffull <= 50) {
			proxy_log(r, "Enable input");
			r->disabled = 0;
		} else {
			return;
		}
	}
	if (cbuf_fill(r->buf, data, datasize) != 0) {
		proxy_log(r, "Disable input, buffer is full.");
		r->disabled = 1;
	}
}

int input_check_state(INPUT *r) {
	if (r->dienow) {
		// proxy_log(r, "Forced disconnect.");
		return 2;
	}
	if (r->reconnect) {
		proxy_log(r, "Forced reconnect.");
		return 1;
	}
	return 0;
}

int process_pat(INPUT *r, uint16_t pid, uint8_t *ts_packet) {
	INPUT_STREAM *s = &r->stream;

	if (pid != 0)
		return 0;

	// Process PAT
	s->pat = ts_pat_push_packet(s->pat, ts_packet);

	
	if (s->last_pat->initialized) {
		if (!s->pat->initialized) return -1;  // Incomplete
		if (!ts_pat_is_same(s->pat, s->last_pat)) {
			proxy_log(r, "========================PAT changed.========================");
			return -2; // Reconnect
		}
		ts_pat_free(&s->last_pat);
		s->last_pat = ts_pat_alloc();
	}
	s->last_pat = ts_pat_push_packet(s->last_pat, ts_packet);
	if (s->pat->initialized) {
		// PMT pid is still unknown
		if (!s->pmt_pid) {
			if (!input_process_pat(r)) {
				proxy_log(r, "Can't parse PAT to find PMT pid.");
				return -2;
			}
		}
		// Rewritten PAT is not yet initialized
		if (!s->pat_rewritten || !s->pat_rewritten->initialized) {
			input_rewrite_pat(r);
#if dump_tables
			proxy_log(r, "PAT found!");
			proxy_log(r, "*** Original PAT ***");
			ts_pat_dump(s->pat);
			proxy_log(r, "*** Rewritten PAT ***");
			ts_pat_dump(s->pat_rewritten);
			pidref_dump(s->pidref);
#endif
		}

		// Only if output file is written
		if (r->ifd && s->pat_rewritten && s->pat_rewritten->initialized) {
			int j;
			struct ts_pat *P = s->pat_rewritten;
			for (j=0;j<P->section_header->num_packets;j++) {
				ts_packet_set_cont(P->section_header->packet_data + (j * TS_PACKET_SIZE), j + s->pid_pat_cont);
			}
			P->ts_header.continuity = s->pid_pat_cont;
			s->pid_pat_cont += P->section_header->num_packets;
			if (write(r->ifd, P->section_header->packet_data, P->section_header->num_packets * TS_PACKET_SIZE) <0) {;}
		}
	}
	// Stuff packet with NULL data
	memset(ts_packet, 0xff, TS_PACKET_SIZE);
	ts_packet[0] = 0x47;
	ts_packet[1] = 0x1F;
	ts_packet[2] = 0xFF;
	ts_packet[3] = 0x10;

	return 1;
}

int process_pmt(INPUT *r, uint16_t pid, uint8_t *ts_packet) {
    if (!pid || pid != r->stream.pmt_pid) return 0;
    ca_pmt_packet(r, ts_packet);
    return -1;
}

static void emit_input_eit(void *opaque, const uint8_t *packets, size_t len) {
    INPUT *r = opaque;
    if (!r->dienow && !r->disabled && config->output && config->output->psibuf)
        cbuf_fill(config->output->psibuf, (uint8_t *)packets, (int)len);
}

int process_eit(INPUT *r, uint8_t *ts_packet) {
    if (r->channel->eit_mode != 1) return -1;
    eit_forward_packet(&r->stream.eit_forward, ts_packet,
        r->stream.input_service_id, r->channel->service_id,
        config->transport_stream_id, config->original_network_id, emit_input_eit, r);
    return 1;
}

int in_worktime(int start, int end) {
	if (!start && !end)
		return 1;
	struct tm ltime;
	time_t timep = time(NULL);
	localtime_r(&timep, &ltime);
	int seconds = ltime.tm_sec + ltime.tm_min * 60 + ltime.tm_hour * 3600;
	if (start > end) {
		if (start >= seconds && end < seconds)
			return 0;
		else
			return 1;
	} else {
		if (start <= seconds && end > seconds)
			return 1;
		else
			return 0;
	}
	return 1;
}

void * input_stream(void *self) {
	INPUT *r = self;
	INPUT_STREAM *s = &r->stream;
	char buffer[RTP_HEADER_SIZE + FRAME_PACKET_SIZE];
	char *buf = buffer + RTP_HEADER_SIZE;

	signal(SIGPIPE, SIG_IGN);

	proxy_log(r, "Start");
	r->working = in_worktime(r->channel->worktime_start, r->channel->worktime_end);
	if (!r->working)
		proxy_log(r, "Worktime has not yet begin, sleeping.");

	int http_code = 0;
	while (keep_going) {
		if (input_check_state(r) == 2) // r->dienow is on
			goto QUIT;

		while (!r->working) {
			usleep(250000);
			r->working = in_worktime(r->channel->worktime_start, r->channel->worktime_end);
			if (r->working)
				proxy_log(r, "Worktime started.");
			if (!keep_going)
				goto QUIT;
		}

		r->working = in_worktime(r->channel->worktime_start, r->channel->worktime_end);

		int result = connect_source(self, 1, FRAME_PACKET_SIZE * 1000, &http_code);
		if (result != 0)
			goto RECONNECT;

		channel_source sproto = get_sproto(r->channel->source);
		int rtp = is_rtp(r->channel->source);

		if (!rtp && mpeg_sync(r, sproto) != 0) {
			proxy_log(r, "Can't sync input MPEG TS");
			sleep(2);
			goto RECONNECT;
		}

		ssize_t readen;
		int max_zero_reads = MAX_ZERO_READS;

		// Reset all stream parameters on reconnect.
		input_stream_reset(r);

		for (;;) {
			r->working = in_worktime(r->channel->worktime_start, r->channel->worktime_end);
			if (!r->working) {
				proxy_log(r, "Worktime ended.");
				goto STOP;
			}

			switch (input_check_state(r)) {
				case 1: goto RECONNECT;		// r->reconnect is on
				case 2: goto QUIT;			// r->dienow is on
			}

			if (sproto == tcp_sock) {
				readen = fdread_ex(r->sock, buf, FRAME_PACKET_SIZE, TCP_READ_TIMEOUT, TCP_READ_RETRIES, 1);
			} else {
				if (!rtp) {
					readen = fdread_ex(r->sock, buf, FRAME_PACKET_SIZE, UDP_READ_TIMEOUT, UDP_READ_RETRIES, 0);
				} else {
					readen = fdread_ex(r->sock, buffer, FRAME_PACKET_SIZE + RTP_HEADER_SIZE, UDP_READ_TIMEOUT, UDP_READ_RETRIES, 0);
					if (readen > RTP_HEADER_SIZE)
						readen -= RTP_HEADER_SIZE;
				}
			}

			if (readen < 0)
				goto RECONNECT;
			if (readen == 0) { // ho, hum, wtf is going on here?
				proxy_log(r, "Zero read, continuing...");
				if (--max_zero_reads == 0) {
					proxy_log(r, "Max zero reads reached, reconnecting.");
					break;
				}
				continue;
			}

			ca_input_tick(r);
			int i;
			for (i=0; i + 188 <= readen; i+=188) {

				if (r->dienow)
					goto QUIT;
				uint8_t *ts_packet = (uint8_t *)buf + i;
				uint16_t pid = ts_packet_get_pid(ts_packet);

				if (pid == 1) {
                    ca_cat_packet(r, ts_packet);
                    continue;
                }

                int pat_result = process_pat(r, pid, ts_packet);
				if (pat_result == -2)
					goto RECONNECT;
				if (pat_result < 0) // PAT incomplete
					continue;

				int pmt_result = process_pmt(r, pid, ts_packet);
				if (pmt_result == -2)
					goto RECONNECT;
				if (pmt_result < 0) // PMT rewritten or incomplete
					continue;

				pid = ts_packet_get_pid(ts_packet);

				if (pid == 0x12) // EIT
				{
					process_eit(r, ts_packet);
					continue;
				}

				// Kill incoming NIT, SDT, RST, TDT/TOT
				if (pid == s->nit_pid || pid == 0x10 || pid == 0x11 || pid == 0x13 || pid == 0x14 || pid >= 0x1fff) {
					// LOGf("INPUT: %-10s: Remove PID %03x\n", r->channel->id, pid);
					continue;
				}

				// Do we have PAT and PMT? (if we have pmt we have PAT, so check only for PMT)
				if (s->pmt_rewritten && pid == s->pcr_pid && ts_packet_has_pcr(ts_packet)) {
					s->input_pcr = ts_packet_get_pcr(ts_packet);
					// LOGf("INPUT : [%-12s] PCR: %llu\n", r->channel->id, s->input_pcr);
				}

				// Yes, we have enough data to start outputing
				if (s->input_pcr) {
					if (ca_shared_emm_packet(r, pid, ts_packet)) continue;
					/* Pass only declared ES/PCR and CA PIDs. CAS uses a separate,
                     * per-input output range so foreign PIDs cannot collide. */
                    uint16_t mapped = ca_packet_pid(r, pid);
                    if (!mapped) mapped = pidref_get_new_pid(s->pidref, pid);
                    if (!mapped) continue;
                    ts_packet_set_pid(ts_packet, mapped);
					input_buffer_add(r, ts_packet, TS_PACKET_SIZE);
					if (!r->input_ready)
						r->input_ready = 1;
				}
			}

			max_zero_reads = MAX_ZERO_READS;
		}
		proxy_log(r, "fdread timeout");
RECONNECT:
        ca_input_reset(r);
		proxy_log(r, "Reconnect");
		shutdown_fd(&(r->sock));
		chansrc_next(r->channel);
		continue;
STOP:
        ca_input_reset(r);
		proxy_log(r, "Stop");
		shutdown_fd(&(r->sock));
		continue;
QUIT:
        ca_input_reset(r);
		break;
	}
	proxy_close(config->inputs, &r);

	return 0;
}
