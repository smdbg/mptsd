#!/usr/bin/env python3
"""CAS remultiplexing integration tests using synthetic, localhost-only MPEG-TS.

No cards, keys, DVB adapters, external streams or running headend are used.
Expected output PIDs are discovered from emitted tables, never from allocator
internals. Private section payloads are opaque bytes, not actual ECM/EMM.
"""
import collections
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

from mptsd_protocol_test import SOURCE, crc, eit, packets


def psi(table, extension, body, version=0, number=0, last=0, current=True):
    length = len(body) + 9
    value = bytes([table, 0xb0 | (length >> 8), length & 255])
    value += struct.pack('>HBBB', extension, 0xc0 | (version << 1) | current, number, last)
    value += body
    return value + struct.pack('>I', crc(value))


def descriptor(caid, pid, private=b''):
    return bytes([9, 4 + len(private)]) + struct.pack('>HH', caid, 0xe000 | pid) + private


def descs(value):
    result = []
    while value:
        assert len(value) >= 2 and len(value) >= value[1] + 2, 'Malformed output descriptors'
        size = value[1] + 2
        result.append(value[:size])
        value = value[size:]
    return result


def cas(value):
    result = []
    for d in descs(value):
        if d[0] == 9:
            assert len(d) >= 6, 'Short output CA descriptor'
            result.append((int.from_bytes(d[2:4], 'big'), int.from_bytes(d[4:6], 'big') & 8191, d[6:]))
    return result


def private_section(table, marker, size=410):
    body = (marker + bytes(range(256)) * 4)[:size]
    return bytes([table, 0x70 | (len(body) >> 8), len(body) & 255]) + body


def ts_packets(data):
    assert len(data) % 188 == 0
    for start in range(0, len(data), 188):
        packet = data[start:start+188]
        assert packet[0] == 0x47, 'Lost TS alignment'
        yield packet


def pid_of(packet):
    return ((packet[1] & 31) << 8) | packet[2]


def sections(data, pid, check_crc=True, check_cc=False):
    """Assemble PSI/private sections, including pointer tails and packed sections."""
    output, pending, previous = [], bytearray(), None

    def consume(value, start_allowed):
        nonlocal pending
        if not pending and not start_allowed:
            return
        pending += value
        while pending:
            if pending[0] == 255:
                pending.clear()
                break
            if len(pending) < 3:
                break
            size = 3 + ((pending[1] & 15) << 8) + pending[2]
            assert size <= 4096, 'Impossible output section size'
            if len(pending) < size:
                break
            section = bytes(pending[:size])
            del pending[:size]
            if check_crc:
                assert crc(section) == 0, 'Bad output PSI CRC'
            output.append(section)

    for packet in ts_packets(data):
        if pid_of(packet) != pid or not packet[3] & 16:
            continue
        cc = packet[3] & 15
        if check_cc and previous is not None:
            assert cc == (previous + 1) % 16, f'PID {pid}: output CC {previous} -> {cc}'
        previous = cc
        offset = 4 + (1 + packet[4] if packet[3] & 32 else 0)
        if offset >= 188:
            continue
        payload = packet[offset:]
        if packet[1] & 64:
            pointer = payload[0]
            assert pointer < len(payload), 'Bad output pointer'
            consume(payload[1:1+pointer], False)
            pending.clear()
            consume(payload[1+pointer:], True)
        else:
            consume(payload, False)
    return output


def pmts(data):
    found = {}
    pats = sections(data, 0)
    for pat in pats:
        for start in range(8, len(pat)-4, 4):
            sid = int.from_bytes(pat[start:start+2], 'big')
            pid = int.from_bytes(pat[start+2:start+4], 'big') & 8191
            if sid:
                tables = sections(data, pid)
                if tables:
                    pmt = tables[-1]
                    size = int.from_bytes(pmt[10:12], 'big') & 4095
                    info = {'pid':pid, 'pcr':int.from_bytes(pmt[8:10], 'big') & 8191,
                            'program':pmt[12:12+size], 'es':[], 'version':(pmt[5] >> 1) & 31}
                    pos = 12 + size
                    while pos < len(pmt)-4:
                        size = int.from_bytes(pmt[pos+3:pos+5], 'big') & 4095
                        info['es'].append((pmt[pos], int.from_bytes(pmt[pos+1:pos+3], 'big') & 8191,
                                           pmt[pos+5:pos+5+size]))
                        pos += 5 + size
                    assert pos == len(pmt)-4, 'Bad output PMT loop'
                    found[sid] = info
    return found


def cat_groups(data):
    """Return complete CAT versions observed, accepting multi-section output."""
    groups, current = [], {}
    for cat in sections(data, 1, check_cc=True):
        assert cat[0] == 1 and cat[5] & 1, 'Not a current CAT'
        key = ((cat[5] >> 1) & 31, cat[7])
        group = current.setdefault(key, {})
        group[cat[6]] = cat
        if set(group) == set(range(cat[7]+1)):
            groups.append((key[0], b''.join(group[n][8:-4] for n in range(cat[7]+1))))
    return groups


def sdt_flags(data):
    result = {}
    for sdt in sections(data, 17):
        pos = 11
        while pos < len(sdt)-4:
            sid = int.from_bytes(sdt[pos:pos+2], 'big')
            result[sid] = bool(sdt[pos+3] & 16)
            pos += 5 + (int.from_bytes(sdt[pos+3:pos+5], 'big') & 4095)
        assert pos == len(sdt)-4
    return result


class SPTS:
    def __init__(self, marker=b'A', input_sid=201):
        self.marker, self.input_sid = marker, input_sid
        self.cc = collections.Counter()
        self.pmt_version = self.cat_version = 0
        self.program, self.es = b'', b''
        self.cat_parts = [b'']
        self.pmt_current = self.cat_current = True
        self.cat_indices = None
        self.cat_bad_crc = False
        self.payloads = {}
        self.ticks = 0
        self.video_pid, self.audio_pid = 256, 257
        self.pmt_override = None
        self.emit_pmt = True

    def packetize(self, value, pid):
        output = packets(value, pid, self.cc[pid])
        self.cc[pid] += len(output)
        return b''.join(output)

    def frame(self):
        data = self.packetize(psi(0, 100, struct.pack('>HH', self.input_sid, 0xe064)), 0)
        pmt_body = struct.pack('>HH', 0xe000 | self.video_pid, 0xf000 | len(self.program)) + self.program
        pmt_body += b'\x1b' + struct.pack('>HH', 0xe000 | self.video_pid, 0xf000 | len(self.es)) + self.es
        pmt_body += b'\x06' + struct.pack('>HH', 0xe000 | self.audio_pid, 0xf000)
        pmt = self.pmt_override or psi(2, self.input_sid, pmt_body, self.pmt_version, current=self.pmt_current)
        if self.emit_pmt:
            data += self.packetize(pmt, 100)
        for index, part in enumerate(self.cat_parts):
            if self.cat_indices is not None and index not in self.cat_indices:
                continue
            cat = psi(1, 0xffff, part, self.cat_version, index, len(self.cat_parts)-1, self.cat_current)
            if self.cat_bad_crc:
                cat = cat[:-1] + bytes([cat[-1] ^ 1])
            data += self.packetize(cat, 1)
        pcr = ((90000 + self.ticks * 1800) << 15) | 0x7e00
        video = bytes([0x47, self.video_pid >> 8, self.video_pid & 255, 0xb0 | (self.cc[self.video_pid] & 15), 7, 0x10])
        data += (video + pcr.to_bytes(6, 'big') + self.marker + b'VIDEO').ljust(188, b'v')
        self.cc[self.video_pid] += 1
        audio = bytes([0x47, self.audio_pid >> 8, self.audio_pid & 255, 0xd0 | (self.cc[self.audio_pid] & 15)])
        data += (audio + self.marker + b'AUDIO').ljust(188, b'a')
        self.cc[self.audio_pid] += 1
        for pid, value in self.payloads.items():
            data += self.packetize(value, pid)
        data += self.packetize(eit(self.input_sid), 18)
        self.ticks += 1
        return data


class RunningMux:
    def __init__(self, channels, channel_options=None, global_options=None):
        self.channels = channels
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)
        self.sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sender.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_IF,socket.inet_aton('127.0.0.1'))
        self.sender.setsockopt(socket.IPPROTO_IP,socket.IP_MULTICAST_LOOP,1)
        self.ports = []
        reservations = []
        for _ in channels:
            reserved = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            reserved.bind(('127.0.0.1', 0))
            self.ports.append(reserved.getsockname()[1]); reservations.append(reserved)
        global_text = '[Global]\nnetwork_id=200\noriginal_network_id=200\n'
        for key,value in (global_options or {}).items(): global_text += f'{key}={value}\n'
        (self.path/'global').write_text(global_text+'[Timeouts]\npat=50\nsdt=50\nnit=50\n')
        text = '[Global]\nprovider_name=CAS Test\ntransport_stream_id=5313\n'
        for (index,sid), port in zip(channels, self.ports):
            text += f'[Channel{index}]\nservice_id={sid}\nid=synthetic{index}\nname=Synthetic {index}\neit_mode=1\nsource=udp://127.0.0.1:{port}\n'
            for key,value in (channel_options or {}).get(index, {}).items():
                text += f'{key}={value}\n'
        (self.path/'channels').write_text(text)
        (self.path/'nit').write_text('[Global]\nnetwork_name=CAS Test\n[Transponder1]\ntransport_stream_id=5313\n'
            'frequency=0530,0000\nmodulation=128-QAM\nsymbol_rate=006,9000\n')
        (self.path/'epg').write_text('')
        self.outfile = self.path/'output.ts'
        self.log = (self.path/'log').open('wb')
        for reserved in reservations: reserved.close()
        self.process = subprocess.Popen([os.environ.get('MPTSD_TEST_BINARY',str(SOURCE/'mptsd')),
            '-g',str(self.path/'global'),'-c',str(self.path/'channels'),'-n',str(self.path/'nit'),
            '-e',str(self.path/'epg'),'-N','-W','-f',str(self.outfile),'-B','4','-m','1','-q'],
            stdout=self.log,stderr=self.log)
        time.sleep(.2)

    def phase(self, streams, duration=1.35, settle=.65, emm_datagrams=()):
        start = time.monotonic()
        offset = None
        while time.monotonic()-start < duration:
            assert self.process.poll() is None, (self.path/'log').read_text()
            for address,data in emm_datagrams: self.sender.sendto(data,address)
            for port, stream in zip(self.ports,streams):
                if stream is None:
                    continue
                data = stream.frame()
                for pos in range(0,len(data),1316):
                    self.sender.sendto(data[pos:pos+1316],('127.0.0.1',port))
            if offset is None and time.monotonic()-start >= settle:
                offset = self.outfile.stat().st_size
                offset -= offset % 188
            time.sleep(.02)
        assert offset is not None
        with self.outfile.open('rb') as capture:
            capture.seek(offset)
            data = capture.read()
        return data[:len(data)//188*188]

    def close(self):
        self.process.terminate()
        try: self.process.wait(timeout=6)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.sender.close(); self.log.close(); self.temp.cleanup()

    def __enter__(self): return self
    def __exit__(self,*_): self.close()


class AutomaticCASTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.environ.get('MPTSD_TEST_BINARY'):
            subprocess.run(['make','-s','-C',str(SOURCE),'-j2','REPRODUCIBLE_BUILD=1',
                            'GIT_VER=a2b6e4d-headend'],check=True)

    def assert_payload(self, data, pid, expected):
        values = sections(data,pid,check_crc=False,check_cc=True)
        self.assertGreater(len(values),2,f'PID {pid}: opaque section missing')
        self.assertEqual(set(values),{expected})

    def assert_absent(self, marker, data):
        self.assertFalse(marker in data, f'Filtered marker {marker!r} appeared in output')

    def assert_epg_and_av(self, data, streams, output_sids):
        mapped = pmts(data)
        events = sections(data,18,check_cc=True)
        self.assertEqual({int.from_bytes(e[3:5],'big') for e in events},set(output_sids))
        for stream,sid in zip(streams,output_sids):
            pmt = mapped[sid]
            video = [p for p in ts_packets(data) if pid_of(p)==pmt['es'][0][1]]
            audio = [p for p in ts_packets(data) if pid_of(p)==pmt['es'][1][1]]
            self.assertTrue(video and all(p[3]&0xc0==0x80 for p in video))
            self.assertTrue(audio and all(p[3]&0xc0==0xc0 for p in audio))
            self.assertTrue(all(stream.marker+b'VIDEO' in p for p in video))
            self.assertTrue(all(p[4:].startswith(stream.marker+b'AUDIO') for p in audio))
            for event in events:
                if int.from_bytes(event[3:5],'big')==sid:
                    self.assertEqual(event[8:12],struct.pack('>HH',5313,200))
                    self.assertEqual(event[14:-4],eit(stream.input_sid)[14:-4])

    def shared_streams(self, private=b'common'):
        streams = [SPTS(b'A',201), SPTS(b'B',202)]
        for stream in streams:
            stream.program = descriptor(0x0b00,98)
            stream.cat_parts = [descriptor(0x0b00,90,private)]
            stream.payloads = {98:private_section(0x80,stream.marker+b'ECM'),
                               90:private_section(0x82,b'COMMON-EMM')}
        options = {i:{'emm_shared_id':'tvcas-test','emm_shared_caid':'0x0b00','emm_shared_pid':90}
                   for i in [1,2]}
        return streams, options

    def test_explicit_shared_emm_single_pid_and_owner_failover(self):
        streams, options = self.shared_streams()
        with RunningMux([(1,1001),(2,1002)], options) as mux:
            before = mux.phase(streams, duration=2, settle=1)
            # Both feeds remain live, but the elected first source stops EMM.
            # The second source must take over complete sections without a CC gap.
            del streams[0].payloads[90]
            failover = mux.phase(streams, duration=3.8, settle=2.6)
            # Removing the old owner's CAT must not remove the shared descriptor.
            streams[0].cat_parts = [b'']; streams[0].cat_version += 1
            removed = mux.phase(streams, duration=2, settle=1)
            # Include the transition itself, not just settled phase snapshots.
            continuous = mux.outfile.read_bytes()
            continuous = continuous[:len(continuous)//188*188]
        for data in [before,failover,removed]:
            emm = cas(cat_groups(data)[-1][1])
            self.assertEqual(len(emm),1)
            self.assertEqual((emm[0][0],emm[0][2]),(0x0b00,b'common'))
            self.assert_payload(data,emm[0][1],streams[1].payloads[90])
            mapped = pmts(data)
            ecms = [cas(mapped[sid]['program'])[0][1] for sid in [1001,1002]]
            self.assertEqual(len(set(ecms+[emm[0][1]])),3)
            self.assert_epg_and_av(data,streams,[1001,1002])
        self.assertEqual([cas(cat_groups(d)[-1][1])[0][1] for d in [before,failover,removed]],
                         [cas(cat_groups(before)[-1][1])[0][1]]*3)
        emm_pid = cas(cat_groups(before)[-1][1])[0][1]
        self.assertEqual(set(sections(continuous,emm_pid,check_crc=False,check_cc=True)),
                         {streams[1].payloads[90]})

    def test_same_caid_distinct_domains_and_unmarked_input_remain_separate(self):
        streams, options = self.shared_streams()
        options[2]['emm_shared_id'] = 'other-card-domain'
        third = SPTS(b'C',203)
        third.program = descriptor(0x0b00,98)
        third.cat_parts = [descriptor(0x0b00,90,b'common')]
        third.payloads = {98:private_section(0x80,b'C-ECM'),90:private_section(0x82,b'OTHER-EMM')}
        with RunningMux([(1,1001),(2,1002),(3,1003)], options) as mux:
            data = mux.phase(streams+[third],duration=2,settle=1)
        emm = cas(cat_groups(data)[-1][1])
        self.assertEqual(len(emm),3)
        self.assertEqual(len({e[1] for e in emm}),3)
        values = [set(sections(data,e[1],check_crc=False,check_cc=True)) for e in emm]
        self.assertEqual(values.count({streams[0].payloads[90]}),2)
        self.assertEqual(values.count({third.payloads[90]}),1)

    def test_shared_emm_rejects_conflicting_descriptor_and_ecm_pid(self):
        streams, options = self.shared_streams()
        streams[1].cat_parts = [descriptor(0x0b00,90,b'foreign-domain')]
        streams[1].payloads[90] = private_section(0x82,b'CONFLICTING-EMM')
        with RunningMux([(1,1001),(2,1002)], options) as mux:
            data = mux.phase(streams,duration=2,settle=1)
            self.assertIn('Shared EMM descriptor conflict',(mux.path/'log').read_text())
            streams[0].program = descriptor(0x0b00,90)
            streams[0].pmt_version += 1
            changed = mux.phase(streams,duration=2,settle=1)
        self.assertEqual(len(cas(cat_groups(data)[-1][1])),1)
        self.assert_absent(b'CONFLICTING-EMM',data)
        # Invalid PMT never aliases EMM to ECM; last valid ECM mapping stays.
        self.assertEqual(cas(pmts(data)[1001]['program']),cas(pmts(changed)[1001]['program']))

    def test_shared_emm_reassembles_full_size_sections_with_clean_cc(self):
        streams, options = self.shared_streams(private=b'')
        value = b'\x82\x7f\xfd' + bytes(range(256))*15 + bytes(range(253))
        self.assertEqual(len(value),4096)
        for stream in streams: stream.payloads[90] = value
        with RunningMux([(1,1001),(2,1002)], options) as mux:
            data = mux.phase(streams,duration=2,settle=1)
        emm = cas(cat_groups(data)[-1][1])
        self.assertEqual(len(emm),1)
        self.assert_payload(data,emm[0][1],value)

    def test_shared_emm_configuration_validation_and_maximum_group_count(self):
        cases = [{'emm_shared_id':'missing-fields'},
                 {'emm_shared_id':'bad identity','emm_shared_caid':2816,'emm_shared_pid':90},
                 {'emm_shared_id':'x'*65,'emm_shared_caid':2816,'emm_shared_pid':90},
                 {'emm_shared_id':'valid','emm_shared_caid':2816,'emm_shared_pid':8191}]
        for values in cases:
            with self.subTest(values=values), RunningMux([(1,1001)],{1:values}) as mux:
                mux.process.wait(timeout=3)
                self.assertNotEqual(mux.process.returncode,0)
                self.assertIn('invalid shared EMM',(mux.path/'log').read_text())
        _, options = self.shared_streams()
        options[2]['emm_shared_caid'] = 0x0900
        with RunningMux([(1,1001),(2,1002)],options) as mux:
            mux.process.wait(timeout=3)
            self.assertNotEqual(mux.process.returncode,0)
            self.assertIn('conflicting CAID',(mux.path/'log').read_text())
        options = {i:{'emm_shared_id':f'domain-{i}','emm_shared_caid':2816,'emm_shared_pid':90}
                   for i in range(1,32)}
        with RunningMux([(i,1000+i) for i in range(1,32)],options) as mux:
            self.assertIsNone(mux.process.poll())

    def common_feed(self):
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reservation:
            reservation.bind(('127.0.0.1',0))
            port = reservation.getsockname()[1]
        return {'emm_shared_id':'tvcas-test','emm_shared_caid':2816,
                'emm_shared_group':'239.255.254.17','emm_shared_port':port,
                'emm_shared_interface':'127.0.0.1'}

    def test_global_emm_in_clear_transport_and_no_replay_on_feed_loss(self):
        feed = self.common_feed()
        destination = (feed['emm_shared_group'],feed['emm_shared_port'])
        emm = private_section(0x82,b'GLOBAL-EMM')
        stream = SPTS()
        with RunningMux([(1,1001)],global_options=feed) as mux:
            before = mux.phase([stream],duration=2,settle=1,emm_datagrams=[(destination,emm)])
            stopped = mux.phase([stream],duration=1.6,settle=.8)
        entries = cas(cat_groups(before)[-1][1])
        self.assertEqual(len(entries),1)
        self.assertEqual((entries[0][0],entries[0][2]),(2816,b''))
        self.assert_payload(before,entries[0][1],emm)
        self.assert_absent(b'GLOBAL-EMM',stopped)
        self.assertEqual(cas(cat_groups(stopped)[-1][1]),entries)
        self.assertEqual(sdt_flags(before),{1001:False})
        self.assert_epg_and_av(before,[stream],[1001])

    def test_global_emm_suppresses_input_copies_and_rejects_foreign_or_malformed_datagrams(self):
        streams, options = self.shared_streams(private=b'')
        for stream in streams: stream.payloads[90] = private_section(0x82,b'INPUT-COPY')
        feed = self.common_feed()
        destination = (feed['emm_shared_group'],feed['emm_shared_port'])
        emm = private_section(0x82,b'GLOBAL-EMM')
        invalid_prefix = private_section(0x82,b'PARTIAL-DATAGRAM') + b'\x82\x70\xffbad'
        datagrams = [(destination,emm),(destination,invalid_prefix),
                     (destination,private_section(0x80,b'WRONG-TABLE')),
                     (destination,private_section(0x82,b'OVERSIZED-DATAGRAM')+b'\xff'*8193),
                     (('239.255.254.18',feed['emm_shared_port']),private_section(0x82,b'FOREIGN-GROUP'))]
        with RunningMux([(1,1001),(2,1002)],options,feed) as mux:
            data = mux.phase(streams,duration=2,settle=1,emm_datagrams=datagrams)
            no_feed = mux.phase(streams,duration=1.6,settle=.8)
        entries = cas(cat_groups(data)[-1][1])
        self.assertEqual(len(entries),1)
        self.assert_payload(data,entries[0][1],emm)
        for marker in [b'INPUT-COPY',b'PARTIAL-DATAGRAM',b'WRONG-TABLE',b'FOREIGN-GROUP',b'OVERSIZED-DATAGRAM']:
            self.assert_absent(marker,data)
        self.assert_absent(b'INPUT-COPY',no_feed)
        self.assert_absent(b'GLOBAL-EMM',no_feed)
        self.assert_epg_and_av(data,streams,[1001,1002])

    def test_global_emm_one_publication_reaches_multiple_mux_processes(self):
        feed = self.common_feed()
        destination = (feed['emm_shared_group'],feed['emm_shared_port'])
        emm = private_section(0x82,b'ALL-TRANSPORTS')
        with RunningMux([(1,1001)],global_options=feed) as first, \
             RunningMux([(1,2001)],global_options=feed) as second, \
             ThreadPoolExecutor(max_workers=1) as worker:
            other = worker.submit(second.phase,[SPTS(b'B')],duration=2,settle=1)
            left = first.phase([SPTS(b'A')],duration=2,settle=1,emm_datagrams=[(destination,emm)])
            right = other.result(timeout=5)
        for data in [left,right]:
            entries = cas(cat_groups(data)[-1][1])
            self.assertEqual(len(entries),1)
            self.assert_payload(data,entries[0][1],emm)

    def test_global_emm_configuration_rejects_incomplete_or_unicast_feed(self):
        for invalid in [{'emm_shared_id':'test'},
                        {**self.common_feed(),'emm_shared_group':'127.0.0.1'},
                        {**self.common_feed(),'emm_shared_port':65536}]:
            with self.subTest(values=invalid), RunningMux([(1,1001)],global_options=invalid) as mux:
                mux.process.wait(timeout=3)
                self.assertNotEqual(mux.process.returncode,0)
                self.assertIn('Invalid global shared EMM',(mux.path/'log').read_text())

    def test_global_descriptor_fits_beside_31_maximum_input_cats(self):
        class InitialCAT(SPTS):
            def frame(self):
                if self.ticks >= 10: self.cat_indices = []
                return super().frame()
        streams, expected = [], {(2816,b'')}
        for index in range(31):
            stream = InitialCAT(bytes([65+index]),201+index)
            stream.program = descriptor(2816,98)
            values = []
            for number in range(16):
                private = bytes([index,number])+bytes(range(248))
                value = descriptor(2816,90,private)
                self.assertEqual(len(value),256)
                values.append(value); expected.add((2816,private))
            stream.cat_parts = [b''.join(values[n:n+3]) for n in range(0,16,3)]
            self.assertEqual(sum(map(len,stream.cat_parts)),4096)
            streams.append(stream)
        with RunningMux([(n+1,1001+n) for n in range(31)],global_options=self.common_feed()) as mux:
            data = mux.phase(streams,duration=4,settle=2)
        groups = cat_groups(data)
        self.assertTrue(groups,'Maximum CAT with global EMM never completed')
        self.assertEqual({(d[0],d[2]) for d in cas(groups[-1][1])},expected)

    def test_two_inputs_ecm_emm_collisions_private_data_and_epg(self):
        streams = [SPTS(b'A',201), SPTS(b'B',202)]
        for stream in streams:
            stream.program = b'\x52\x01\x05'+descriptor(0x0b00,98,stream.marker+b'program')+descriptor(0x0900,98,b'same-pid')
            stream.es = descriptor(0x0b00,2098,stream.marker+b'es')
            # EMM192 collides with the second program's output PMT, and both
            # inputs use the same original ECM/EMM IDs with different bytes.
            stream.cat_parts = [b'\x05\x04TEST'+descriptor(0x0b00,192,stream.marker+b'emm')+descriptor(0x0900,192,b'shared')]
            stream.payloads = {98:private_section(0x80,stream.marker+b'ECM1'),
                               2098:private_section(0x81,stream.marker+b'ECM2'),
                               192:private_section(0x82,stream.marker+b'EMM'),
                               77:private_section(0x83,b'UNREFERENCED')}
        with RunningMux([(1,1001),(6,1002)]) as mux:
            data = mux.phase(streams)
        mapped = pmts(data)
        self.assertEqual(set(mapped),{1001,1002})
        reserved = {0,1,16,17,18,20,8191}
        for value in mapped.values(): reserved |= {value['pid'],value['pcr']} | {es[1] for es in value['es']}
        all_ca = []
        for stream,sid in zip(streams,[1001,1002]):
            pmt = mapped[sid]
            self.assertIn(b'\x52\x01\x05',descs(pmt['program']))
            pc, ec = cas(pmt['program']), cas(pmt['es'][0][2])
            self.assertEqual([(c[0],c[2]) for c in pc],[(0x0b00,stream.marker+b'program'),(0x0900,b'same-pid')])
            self.assertEqual(pc[0][1],pc[1][1])
            self.assertEqual([(c[0],c[2]) for c in ec],[(0x0b00,stream.marker+b'es')])
            for original,output in [(98,pc[0][1]),(2098,ec[0][1])]:
                self.assert_payload(data,output,stream.payloads[original]);all_ca.append(output)
        cats = cat_groups(data)
        self.assertTrue(cats)
        # CAT private descriptor contexts from unrelated inputs cannot be
        # merged safely; only CA descriptors are published in the common CAT.
        self.assertNotIn(b'\x05\x04TEST',descs(cats[-1][1]))
        emm = cas(cats[-1][1])
        self.assertEqual(len(emm),4)
        for stream in streams:
            entry = next(d for d in emm if d[2]==stream.marker+b'emm')
            self.assert_payload(data,entry[1],stream.payloads[192]);all_ca.append(entry[1])
        self.assertEqual(len(all_ca),len(set(all_ca)))
        self.assertFalse(set(all_ca)&reserved)
        self.assert_absent(b'UNREFERENCED',data)
        self.assertEqual(sdt_flags(data),{1001:True,1002:True})
        self.assert_epg_and_av(data,streams,[1001,1002])

    def test_pmt_cat_updates_and_removal_retire_old_routes(self):
        stream = SPTS()
        stream.program = descriptor(0x0b00,98,b'first')
        stream.cat_parts = [descriptor(0x0b00,90,b'first')]
        stream.payloads = {98:private_section(0x80,b'OLD-ECM'),90:private_section(0x82,b'OLD-EMM'),
                           2098:private_section(0x81,b'NEW-ECM'),91:private_section(0x82,b'NEW-EMM')}
        with RunningMux([(1,5011)]) as mux:
            before = mux.phase([stream])
            stream.program = descriptor(0x0b00,2098,b'second')
            stream.cat_parts = [descriptor(0x0b00,91,b'second')]
            # Broken upstreams sometimes change contents without incrementing
            # version. Output must detect the bytes and publish a new version.
            stream.pmt_version = stream.cat_version = 0
            after = mux.phase([stream],duration=1.8,settle=.9)
            stream.program = b'';stream.cat_parts = [b'']
            stream.pmt_version = stream.cat_version = 2
            clear = mux.phase([stream],duration=1.8,settle=.9)
        p0,p1,p2 = [pmts(d)[5011] for d in [before,after,clear]]
        c0,c1,c2 = [cat_groups(d)[-1] for d in [before,after,clear]]
        self.assertNotEqual(p0['version'],p1['version']);self.assertNotEqual(p1['version'],p2['version'])
        self.assertNotEqual(c0[0],c1[0]);self.assertNotEqual(c1[0],c2[0])
        self.assert_payload(after,cas(p1['program'])[0][1],stream.payloads[2098])
        self.assert_payload(after,cas(c1[1])[0][1],stream.payloads[91])
        self.assert_absent(b'OLD-ECM',after);self.assert_absent(b'OLD-EMM',after)
        self.assertEqual(cas(p2['program']),[]);self.assertEqual(cas(c2[1]),[])
        for marker in [b'OLD-ECM',b'OLD-EMM',b'NEW-ECM',b'NEW-EMM']:
            self.assert_absent(marker,clear)
        self.assertEqual(sdt_flags(before),{5011:True});self.assertEqual(sdt_flags(clear),{5011:False})
        self.assert_epg_and_av(clear,[stream],[5011])

    def test_incomplete_cat_version_never_replaces_complete_table(self):
        stream = SPTS()
        stream.program = descriptor(0x0b00,98)
        stream.cat_parts = [descriptor(0x0b00,90,b'old-a'),descriptor(0x0100,91,b'old-b')]
        stream.cat_indices = {0}
        stream.payloads = {98:private_section(0x80,b'ECM'),90:private_section(0x82,b'OLD-A'),
                           91:private_section(0x82,b'OLD-B'),92:private_section(0x82,b'NEW-A'),93:private_section(0x82,b'NEW-B')}
        with RunningMux([(1,5011)]) as mux:
            partial = mux.phase([stream])
            stream.cat_indices = None
            complete = mux.phase([stream])
            stream.cat_version = 1
            stream.cat_parts = [descriptor(0x0b00,92,b'new-a'),descriptor(0x0100,93,b'new-b')]
            stream.cat_indices = {1}
            changed_partial = mux.phase([stream])
            stream.cat_indices = None
            # PCR mode1's output buffers can retain old packets for~750ms.
            # Observe after that drain, rather than interpreting queued bytes
            # as a still-active route (the new CAT is published separately).
            changed_complete = mux.phase([stream],duration=2,settle=1)
        self.assertFalse(any(cas(value) for _,value in cat_groups(partial)))
        for data in [complete,changed_partial]:
            self.assertEqual({c[2] for c in cas(cat_groups(data)[-1][1])},{b'old-a',b'old-b'})
        final = cas(cat_groups(changed_complete)[-1][1])
        self.assertEqual({c[2] for c in final},{b'new-a',b'new-b'})
        for c,original in [(next(c for c in final if c[2]==b'new-a'),92),
                           (next(c for c in final if c[2]==b'new-b'),93)]:
            self.assert_payload(changed_complete,c[1],stream.payloads[original])
        self.assert_absent(b'OLD-A',changed_complete);self.assert_absent(b'OLD-B',changed_complete)

    def test_future_and_malformed_updates_keep_valid_mapping(self):
        stream = SPTS()
        stream.program = descriptor(0x0b00,98,b'valid')
        stream.cat_parts = [descriptor(0x0b00,90,b'valid')]
        stream.payloads = {98:private_section(0x80,b'VALID-ECM'),90:private_section(0x82,b'VALID-EMM'),
                           99:private_section(0x80,b'INVALID-ECM'),91:private_section(0x82,b'INVALID-EMM')}
        with RunningMux([(1,5011)]) as mux:
            initial = mux.phase([stream])
            stream.pmt_current = stream.cat_current = False
            stream.pmt_version = stream.cat_version = 1
            stream.program = descriptor(0x0b00,99,b'future')
            stream.cat_parts = [descriptor(0x0b00,91,b'future')]
            future = mux.phase([stream])
            stream.pmt_current = stream.cat_current = True
            stream.program = b'\x09\x04\x0b'
            stream.cat_parts = [b'\x09\x04\x0b']
            truncated = mux.phase([stream])
            stream.program = descriptor(0x0b00,98,b'valid')
            stream.pmt_version = 0
            stream.cat_parts = [descriptor(0x0b00,91,b'bad-crc')]
            stream.cat_bad_crc = True
            bad_crc = mux.phase([stream])
            # The PMT CRC is valid but its elementary stream loop overclaims
            # bytes. Validation must happen before the old PMT parser reads it.
            stream.pmt_override = psi(2,201,b'\xe1\x00\xf0\x00\x1b\xe1\x00\xf0\x64',2)
            bad_es_length = mux.phase([stream])
        initial_pmt = pmts(initial)[5011]
        initial_cat = cat_groups(initial)[-1]
        for data in [future,truncated,bad_crc,bad_es_length]:
            self.assertEqual(cas(pmts(data)[5011]['program']),cas(initial_pmt['program']))
            self.assertEqual(cas(cat_groups(data)[-1][1]),cas(initial_cat[1]))
            self.assert_payload(data,cas(initial_pmt['program'])[0][1],stream.payloads[98])
            self.assert_payload(data,cas(initial_cat[1])[0][1],stream.payloads[90])
            self.assert_absent(b'INVALID-ECM',data);self.assert_absent(b'INVALID-EMM',data)

    def test_reconnect_withdraws_cat_and_relearns_same_version(self):
        stream = SPTS()
        stream.program = descriptor(0x0b00,98)
        stream.cat_parts = [descriptor(0x0b00,90,b'before-disconnect')]
        stream.payloads = {98:private_section(0x80,b'ECM'),90:private_section(0x82,b'OLD-EMM')}
        with RunningMux([(1,5011)]) as mux:
            before = mux.phase([stream])
            disconnected = mux.phase([None],duration=6,settle=4.8)
            stream.cat_parts = [descriptor(0x0b00,91,b'after-reconnect')]
            stream.payloads[91] = private_section(0x82,b'NEW-EMM')
            resumed = mux.phase([stream],duration=2.4,settle=1.4)
        self.assertTrue(cas(cat_groups(before)[-1][1]))
        self.assertEqual(cas(cat_groups(disconnected)[-1][1]),[])
        final = cas(cat_groups(resumed)[-1][1])
        self.assertEqual([(d[0],d[2]) for d in final],[(0x0b00,b'after-reconnect')])
        self.assert_payload(resumed,final[0][1],stream.payloads[91])
        self.assert_absent(b'OLD-EMM',resumed)
        self.assert_epg_and_av(resumed,[stream],[5011])

    def test_same_ca_pid_shared_by_program_es_and_cat(self):
        stream = SPTS()
        stream.program = descriptor(0x0b00,98,b'program')
        stream.es = descriptor(0x0900,98,b'es')
        stream.cat_parts = [descriptor(0x0100,98,b'emm')]
        stream.payloads = {98:private_section(0x82,b'SHARED-PRIVATE')}
        with RunningMux([(31,6011)]) as mux:
            data = mux.phase([stream])
        pmt = pmts(data)[6011]
        refs = [cas(pmt['program'])[0],cas(pmt['es'][0][2])[0],cas(cat_groups(data)[-1][1])[0]]
        self.assertEqual(len({d[1] for d in refs}),1)
        self.assertEqual({d[2] for d in refs},{b'program',b'es',b'emm'})
        self.assert_payload(data,refs[0][1],stream.payloads[98])
        self.assert_epg_and_av(data,[stream],[6011])

    def test_reserved_or_same_input_media_ca_pid_cannot_replace_good_table(self):
        stream = SPTS()
        stream.program = descriptor(0x0b00,98,b'valid')
        stream.cat_parts = [descriptor(0x0b00,90,b'valid')]
        stream.payloads = {98:private_section(0x80,b'VALID-ECM'),90:private_section(0x82,b'VALID-EMM')}
        with RunningMux([(1,5011)]) as mux:
            initial = mux.phase([stream])
            for bad_pid in [0,1,17,8191,100,256,257]:
                with self.subTest(pid=bad_pid):
                    stream.program = descriptor(0x0b00,bad_pid,b'invalid')
                    stream.cat_parts = [descriptor(0x0b00,bad_pid,b'invalid')]
                    stream.pmt_version = stream.cat_version = 1
                    data = mux.phase([stream])
                    self.assertEqual(cas(pmts(data)[5011]['program']),cas(pmts(initial)[5011]['program']))
                    self.assertEqual(cas(cat_groups(data)[-1][1]),cas(cat_groups(initial)[-1][1]))
                    self.assert_payload(data,cas(pmts(initial)[5011]['program'])[0][1],stream.payloads[98])
                    self.assert_payload(data,cas(cat_groups(initial)[-1][1])[0][1],stream.payloads[90])

    def test_capacity_exhaustion_is_atomic_and_never_reuses_old_pid(self):
        stream = SPTS()
        # Fill128 unique CA slots (127ECM +1EMM); the PMT still fits one legal
        # 1024-byte section. Only two sample private PIDs are transmitted.
        stream.program = b''.join(descriptor(0x0b00,pid) for pid in range(2000,2127))
        stream.cat_parts = [descriptor(0x0b00,90,b'original')]
        stream.payloads = {2000:private_section(0x80,b'CAPACITY-ECM'),90:private_section(0x82,b'CAPACITY-EMM'),
                           2200:private_section(0x80,b'EXCESS-ECM'),91:private_section(0x82,b'EXCESS-EMM')}
        with RunningMux([(1,5011)]) as mux:
            initial = mux.phase([stream],duration=1.6,settle=.8)
            stream.program = descriptor(0x0b00,2200)
            stream.cat_parts = [descriptor(0x0b00,91,b'excess')]
            stream.pmt_version = stream.cat_version = 1
            excess = mux.phase([stream])
            stream.program = b'';stream.cat_parts = [b'']
            stream.pmt_version = stream.cat_version = 2
            empty = mux.phase([stream])
            stream.program = descriptor(0x0b00,2200)
            stream.cat_parts = [descriptor(0x0b00,91,b'excess')]
            stream.pmt_version = stream.cat_version = 3
            after_removal = mux.phase([stream])
        pmt0 = pmts(initial)[5011]
        self.assertEqual(len(cas(pmt0['program'])),127)
        self.assertEqual(cas(pmts(excess)[5011]['program']),cas(pmt0['program']))
        self.assertEqual(cas(cat_groups(excess)[-1][1]),cas(cat_groups(initial)[-1][1]))
        self.assertEqual(cas(pmts(empty)[5011]['program']),[])
        self.assertEqual(cas(cat_groups(empty)[-1][1]),[])
        self.assertEqual(cas(pmts(after_removal)[5011]['program']),[])
        self.assertEqual(cas(cat_groups(after_removal)[-1][1]),[])
        for data in [excess,after_removal]:
            self.assert_absent(b'EXCESS-ECM',data);self.assert_absent(b'EXCESS-EMM',data)

    def test_large_merged_cat_has_complete_sections_and_continuity(self):
        streams = [SPTS(b'A',201),SPTS(b'B',202)]
        expected = set()
        for index,stream in enumerate(streams):
            stream.program = descriptor(0x0b00,98)
            stream.payloads[98] = private_section(0x80,stream.marker+b'ECM')
            values = []
            for n in range(6):
                private = stream.marker + bytes([n]) + bytes(range(238))
                values.append(descriptor(0x0b00,300+n,private))
                expected.add((0x0b00,private))
                stream.payloads[300+n] = private_section(0x82,stream.marker+bytes([n])+b'EMM')
            stream.cat_parts = [b''.join(values[:3]),b''.join(values[3:])]
        with RunningMux([(1,1001),(2,1002)]) as mux:
            data = mux.phase(streams,duration=2,settle=1)
        cats = sections(data,1,check_cc=True)
        self.assertTrue(cats and any(cat[7] > 0 for cat in cats))
        self.assertTrue(all(len(cat)<=1024 for cat in cats))
        merged = cas(cat_groups(data)[-1][1])
        self.assertTrue({(d[0],d[2]) for d in merged}==expected, 'Merged CAT descriptors differ')
        self.assertEqual(len({d[1] for d in merged}),12)
        for d in merged:
            stream = streams[d[2][0]-ord('A')]
            self.assert_payload(data,d[1],stream.payloads[300+d[2][1]])

    def test_oversized_media_pid_block_cannot_collide_with_next_pmt(self):
        streams = [SPTS(b'A',201),SPTS(b'B',202)]
        with RunningMux([(1,1001),(2,1002)]) as mux:
            initial = mux.phase(streams)
            body = struct.pack('>HH',0xe100,0xf000)
            #32 distinct media PIDs plus PMT exceeds the31mediaPID block.
            for pid in range(256,288):
                body += b'\x1b'+struct.pack('>HH',0xe000|pid,0xf000)
            streams[0].pmt_override = psi(2,201,body,version=1)
            after = mux.phase(streams)
        before_pmts,after_pmts = pmts(initial),pmts(after)
        self.assertEqual(after_pmts,before_pmts)
        self.assert_epg_and_av(after,streams,[1001,1002])

    def test_merged_cat_larger_than_psi_queue_is_emitted_completely(self):
        streams,expected = [],set()
        for index in range(20):
            stream=SPTS(bytes([65+index]),201+index)
            stream.program=descriptor(0x0b00,98)
            values=[]
            for n in range(16):
                private=bytes([index,n])+bytes(range(238))
                expected.add((0x0b00,private))
                values.append(descriptor(0x0b00,90,private))
            stream.cat_parts=[b''.join(values[n:n+4]) for n in range(0,len(values),4)]
            # The generated CAT is ~79KiB, larger than mptsd's64KiB PSI queue.
            # Each input stays within the documented4096descriptor-byte cap.
            # Keep media/private input small; test table scheduling, not load.
            stream.payloads={98:private_section(0x80,stream.marker+b'ECM',size=16),
                             90:private_section(0x82,stream.marker+b'EMM',size=16)}
            streams.append(stream)
        with RunningMux([(n+1,1001+n) for n in range(20)]) as mux:
            data=mux.phase(streams,duration=3.4,settle=1.4)
        groups=cat_groups(data)
        self.assertTrue(groups,'Large CAT never completed; possible PSI queue starvation')
        merged=cas(groups[-1][1])
        self.assertTrue({(d[0],d[2]) for d in merged}==expected, 'Merged CAT descriptors differ')
        self.assertEqual(len({d[1] for d in merged}),20)

    def test_cat_before_pmt_conflict_is_deferred_then_recovers(self):
        stream=SPTS()
        stream.program=descriptor(0x0b00,98)
        stream.cat_parts=[descriptor(0x0b00,256,b'conflicts-with-video')]
        stream.payloads={98:private_section(0x80,b'ECM'),90:private_section(0x82,b'EMM')}
        stream.emit_pmt=False
        with RunningMux([(1,5011)]) as mux:
            cat_first=mux.phase([stream])
            stream.emit_pmt=True
            inconsistent=mux.phase([stream])
            stream.cat_parts=[descriptor(0x0b00,90,b'coherent')]
            stream.cat_version=1
            recovered=mux.phase([stream])
        for data in [cat_first,inconsistent]:
            self.assertFalse(any(cas(value) for _,value in cat_groups(data)))
            self.assertNotIn(5011,pmts(data))
            self.assert_absent(b'AVIDEO',data)
        self.assertEqual(set(pmts(recovered)),{5011})
        self.assertEqual([d[2] for d in cas(cat_groups(recovered)[-1][1])],[b'coherent'])
        self.assert_epg_and_av(recovered,[stream],[5011])

    def test_coordinated_pmt_cat_pid_role_swap_recovers_in_both_orders(self):
        for first in ['pmt','cat']:
            with self.subTest(first=first):
                stream=SPTS()
                stream.program=descriptor(0x0b00,98)
                stream.cat_parts=[descriptor(0x0b00,90,b'old')]
                stream.payloads={98:private_section(0x80,b'ECM'),90:private_section(0x82,b'OLD-EMM')}
                new_body=struct.pack('>HH',0xe05a,0xf000|len(stream.program))+stream.program
                new_body+=b'\x1b\xe0\x5a\xf0\x00\x06\xe1\x01\xf0\x00'
                new_pmt=psi(2,201,new_body,version=1)
                with RunningMux([(1,5011)]) as mux:
                    before=mux.phase([stream])
                    if first=='pmt': stream.pmt_override=new_pmt
                    else:
                        stream.cat_parts=[descriptor(0x0b00,256,b'new')]
                        stream.cat_version=1
                    half=mux.phase([stream])
                    stream.pmt_override=new_pmt
                    stream.cat_parts=[descriptor(0x0b00,256,b'new')]
                    stream.cat_version=1
                    stream.video_pid=90
                    del stream.payloads[90]
                    stream.payloads[256]=private_section(0x82,b'NEW-EMM')
                    after=mux.phase([stream],duration=1.8,settle=.9)
                self.assertEqual(pmts(half),pmts(before))
                self.assertEqual(cas(cat_groups(half)[-1][1]),cas(cat_groups(before)[-1][1]))
                cat=cas(cat_groups(after)[-1][1])
                self.assertEqual([d[2] for d in cat],[b'new'])
                self.assert_payload(after,cat[0][1],stream.payloads[256])
                self.assertNotEqual(pmts(after)[5011]['version'],pmts(before)[5011]['version'])
                self.assert_epg_and_av(after,[stream],[5011])


if __name__ == '__main__':
    unittest.main()
