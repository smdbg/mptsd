#!/usr/bin/env python3
"""Synthetic EIT and complete mux tests; no DVB hardware, CAM, or external stream."""
import ctypes
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT

def crc(data):
    result = 0xffffffff
    for byte in data:
        result ^= byte << 24
        for _ in range(8):
            result = ((result << 1) ^ (0x04c11db7 if result & 0x80000000 else 0)) & 0xffffffff
    return result

def section(table, sid, body, version=7):
    data = bytes([table, 0xb0 | ((len(body) + 9) >> 8), (len(body) + 9) & 255])
    data += struct.pack('>HBBB', sid, 0xc1 | (version << 1), 0, 0) + body
    return data + struct.pack('>I', crc(data))

def eit(sid=201, table=0x4e, payload=0):
    # Valid event descriptors, including opaque contents that must survive untouched.
    events = b''
    for i in range(payload // 240 + 1):
        text = bytes((j + i) % 256 for j in range(min(240, max(0, payload - i * 240))))
        desc = bytes([0x80, len(text)]) + text
        events += struct.pack('>H', 500 + i) + b'\xea\x60\x12\x00\x00\x01\x00\x00' + struct.pack('>H', 0x8000 | len(desc)) + desc
    return section(table, sid, struct.pack('>HHBB', 100, 200, 0, table) + events)

def packets(data, pid=18, cc=0):
    result = []
    while data:
        first = not result
        take = 183 if first else 184
        payload, data = data[:take], data[take:]
        p = bytes([0x47, (pid >> 8) | (0x40 if first else 0), pid & 255, 0x10 | (cc & 15)])
        p += (b'\0' if first else b'') + payload
        result.append(p.ljust(188, b'\xff'))
        cc += 1
    return result

def sections_from_ts(data, pid):
    result, buf, prev = [], b'', None
    for i in range(0, len(data) - 187, 188):
        p = data[i:i+188]
        if p[0] != 0x47 or ((p[1] & 31) << 8 | p[2]) != pid: continue
        if not p[3] & 16: continue
        cc = p[3] & 15
        if prev is not None and pid == 18:
            assert cc == (prev + 1) % 16, (prev, cc)
        prev = cc
        off = 4 + (1 + p[4] if p[3] & 32 else 0)
        if p[1] & 64:
            assert not buf, 'interleaved/truncated section'
            off += 1 + p[off]
        buf += p[off:]
        if len(buf) >= 3:
            size = 3 + ((buf[1] & 15) << 8 | buf[2])
            if len(buf) >= size:
                sec, buf = buf[:size], b''
                assert crc(sec) == 0, 'bad output CRC'
                result.append(sec)
    return result

class State(ctypes.Structure):
    _fields_ = [('section', ctypes.c_uint8 * 4096), ('used', ctypes.c_size_t),
                ('needed', ctypes.c_size_t), ('last_packet', ctypes.c_uint8 * 188),
                ('cc_valid', ctypes.c_int), ('cc', ctypes.c_int)]

CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t)

class EITTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        path = Path(cls.temp.name) / 'eit.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(SOURCE/'eit_forward.c'), '-o', str(path)], check=True)
        cls.lib = ctypes.CDLL(str(path))
        cls.lib.eit_forward_packet.argtypes = [ctypes.POINTER(State), ctypes.c_void_p,
            ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint16, ctypes.c_uint16, CALLBACK, ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        self.state, self.emitted = State(), []
        self.emit = CALLBACK(lambda _, p, size: self.emitted.append(ctypes.string_at(p, size)))

    def feed(self, p):
        self.lib.eit_forward_packet(ctypes.byref(self.state), p, 201, 1001, 5311, 99, self.emit, None)

    def result(self): return sections_from_ts(b''.join(self.emitted), 18)

    def test_present_and_schedule_preserve_events_map_ids(self):
        for table in [0x4e, 0x50, 0x51, 0x5f]:
            with self.subTest(table=table):
                self.setUp()
                original = eit(table=table, payload=1500)
                for p in packets(original): self.feed(p)
                # Unit assembler leaves output CC to the final mux writer.
                raw = bytearray(b''.join(self.emitted))
                for i in range(len(raw)//188): raw[i*188+3] |= i & 15
                out, = sections_from_ts(raw, 18)
                self.assertEqual(struct.unpack('>H', out[3:5])[0], 1001)
                self.assertEqual(out[8:12], struct.pack('>HH', 5311, 99))
                self.assertEqual(out[14:-4], original[14:-4])
                self.assertEqual(out[5:8], original[5:8])

    def test_foreign_and_other_transport_then_valid_packed_sections(self):
        data = eit(999) + eit(table=0x4f) + eit(table=0x60) + eit()
        self.assertLess(len(data), 183)
        self.feed(packets(data)[0])
        self.assertEqual(len(self.emitted), 1)

    def test_crc_and_cc_gap_recover_next_start(self):
        damaged = bytearray(eit()); damaged[-1] ^= 1
        self.feed(packets(bytes(damaged))[0])
        fragmented = packets(eit(payload=600), cc=1)
        self.feed(fragmented[0])
        for p in fragmented[2:]: self.feed(p)
        self.assertFalse(self.emitted)
        self.feed(packets(eit(), cc=9)[0])
        self.assertEqual(len(self.emitted), 1)

    def test_duplicate_packet_and_pointer_finish(self):
        original = eit(payload=200)
        first = packets(original)[0]
        self.feed(first); self.feed(first)
        remaining = original[183:]
        payload = bytes([len(remaining)]) + remaining + eit()
        self.feed((b'\x47\x40\x12\x11' + payload).ljust(188, b'\xff'))
        self.assertEqual(len(self.emitted), 2)

    def test_maximum_section_length_preserves_all_bytes(self):
        original = section(0x50, 201, struct.pack('>HHBB', 100, 200, 0, 0x50) + b'x' * 4078)
        self.assertEqual(len(original), 4096)
        for p in packets(original): self.feed(p)
        raw = bytearray(b''.join(self.emitted))
        self.assertEqual(len(raw), 23 * 188)
        for i in range(23): raw[i*188+3] |= i & 15
        out, = sections_from_ts(raw, 18)
        self.assertEqual(out[14:-4], original[14:-4])

    def test_invalid_pointer_and_scrambled_payload_rejected(self):
        p = bytearray(packets(eit())[0]); p[4] = 184
        self.feed(bytes(p))
        p = bytearray(packets(eit(), cc=1)[0]); p[3] |= 0x80
        self.feed(bytes(p))
        self.assertFalse(self.emitted)

class StatusState(ctypes.Structure):
    _fields_ = [('path', ctypes.c_char_p), ('interval_ms', ctypes.c_uint),
                ('network_output', ctypes.c_int), ('total_packets', ctypes.c_uint64),
                ('null_packets', ctypes.c_uint64), ('write_errors', ctypes.c_uint64),
                ('sequence', ctypes.c_uint64), ('last_total_packets', ctypes.c_uint64),
                ('last_null_packets', ctypes.c_uint64), ('last_sample_ns', ctypes.c_int64),
                ('last_attempt_ns', ctypes.c_int64)]

class OutputStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        so = Path(cls.temp.name)/'status.so'
        subprocess.run(['cc','-shared','-fPIC','-D_GNU_SOURCE','-O2','-Wall','-Wextra','-Werror',
            str(SOURCE/'output_status.c'),'-o',str(so)],check=True)
        cls.lib=ctypes.CDLL(str(so))
        cls.lib.output_status_account.argtypes=[ctypes.POINTER(StatusState),ctypes.c_int,ctypes.c_uint]
        cls.lib.output_status_publish_at.argtypes=[ctypes.POINTER(StatusState),ctypes.c_int64,ctypes.c_int64]
        cls.lib.output_status_publish_at.restype=ctypes.c_int

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_full_minute_window_successful_frames_and_atomic_replacement(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'status.json'
            state=StatusState(path=os.fsencode(path),interval_ms=60000,
                last_sample_ns=1000000000,last_attempt_ns=1000000000)
            for _ in range(4): self.lib.output_status_account(ctypes.byref(state),1,3)
            self.lib.output_status_account(ctypes.byref(state),0,7)
            self.assertEqual(self.lib.output_status_publish_at(ctypes.byref(state),60000000000,1000),0)
            self.assertFalse(path.exists(),'no invented point before complete first minute')
            self.assertEqual(self.lib.output_status_publish_at(ctypes.byref(state),61000000000,2000),1)
            first=json.loads(path.read_text())
            self.assertEqual(first['sequence'],1)
            self.assertEqual(first['pid'],os.getpid())
            self.assertEqual(first['window_seconds'],60)
            self.assertEqual(first['total_packets'],28)
            self.assertEqual(first['null_packets'],12)
            self.assertEqual(first['write_errors'],1)
            self.assertAlmostEqual(first['occupancy_pct'],100*16/28,places=5)
            self.assertAlmostEqual(first['non_null_mbps'],16*188*8/60/1e6,places=6)
            self.assertEqual(path.stat().st_mode&0o777,0o600)
            old_inode=path.stat().st_ino
            for _ in range(10): self.lib.output_status_account(ctypes.byref(state),1,7)
            self.assertEqual(self.lib.output_status_publish_at(ctypes.byref(state),121000000000,3000),1)
            second=json.loads(path.read_text())
            self.assertEqual(second['sequence'],2)
            self.assertEqual(second['window_total_packets'],70)
            self.assertEqual(second['window_null_packets'],70)
            self.assertEqual(second['occupancy_pct'],0)
            self.assertEqual(second['non_null_mbps'],0)
            self.assertNotEqual(path.stat().st_ino,old_inode)
            self.assertEqual(list(Path(tmp).glob('*.tmp.*')),[])
            self.assertEqual(self.lib.output_status_publish_at(ctypes.byref(state),181000000000,4000),1)
            empty=json.loads(path.read_text())
            self.assertIsNone(empty['occupancy_pct'])
            self.assertEqual(empty['output_mbps'],0)

    def test_failed_publication_retains_counters_and_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent=Path(tmp)/'missing';path=parent/'status.json'
            state=StatusState(path=os.fsencode(path),interval_ms=1000,
                last_sample_ns=1000000000,last_attempt_ns=1000000000)
            self.lib.output_status_account(ctypes.byref(state),1,2)
            self.assertEqual(self.lib.output_status_publish_at(ctypes.byref(state),2000000000,1000),-1)
            self.assertEqual(state.sequence,0)
            parent.mkdir()
            self.assertEqual(self.lib.output_status_publish_at(ctypes.byref(state),3000000000,2000),1)
            point=json.loads(path.read_text())
            self.assertEqual(point['window_seconds'],2)
            self.assertEqual(point['window_total_packets'],7)
            self.assertEqual(point['window_null_packets'],2)
            self.assertEqual(point['sequence'],1)

class MuxIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['make', '-s', '-C', str(SOURCE), '-j2', 'REPRODUCIBLE_BUILD=1',
                        'GIT_VER=a2b6e4d-headend'], check=True)

    def test_atomic_ring_buffer_wrap_and_capacity(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(Path(tmp) / 'buffer-test')
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(SOURCE),
                str(ROOT/'tests/mptsd_buffer_test.c'), str(SOURCE/'libfuncs/libfuncs.a'),
                '-pthread', '-o', binary], check=True)
            subprocess.run([binary], check=True)

    def test_si_utf8_encoding_and_capacity(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(Path(tmp) / 'si-test')
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(SOURCE),
                str(ROOT/'tests/mptsd_si_test.c'), str(SOURCE/'libtsfuncs/libtsfuncs.a'),
                '-o', binary], check=True)
            subprocess.run([binary], check=True)

    def test_default_status_interval_is_ten_seconds(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as reserve:
                reserve.bind(('127.0.0.1',0)); port=reserve.getsockname()[1]
            (tmp/'global').write_text(f'[Global]\nnetwork_id=200\nstatus_file={tmp}/status.json\n')
            (tmp/'channels').write_text(f'[Global]\nprovider_name=DVB24\ntransport_stream_id=5311\n[Channel1]\nservice_id=201\nid=default-status\nname=Default status\neit_mode=1\nsource=udp://127.0.0.1:{port}\n')
            (tmp/'nit').write_text('[Global]\nnetwork_name=DVB24\n[Transponder1]\ntransport_stream_id=5311\nfrequency=0514,0000\nmodulation=128-QAM\nsymbol_rate=006,9000\n')
            (tmp/'epg').write_text('')
            command=[str(SOURCE/'mptsd'),'-g',str(tmp/'global'),'-c',str(tmp/'channels'),
                '-n',str(tmp/'nit'),'-e',str(tmp/'epg'),'-N','-W','-f','/dev/null','-B','2','-q']
            process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            try:
                started=time.monotonic()
                time.sleep(9.3)
                self.assertIsNone(process.poll())
                self.assertFalse((tmp/'status.json').exists(),'default must not publish before10seconds')
                while not (tmp/'status.json').exists() and time.monotonic()-started<12:
                    time.sleep(.05)
                point=json.loads((tmp/'status.json').read_text())
                self.assertEqual(point['sequence'],1)
                self.assertEqual(point['pid'],process.pid)
                self.assertGreaterEqual(point['window_seconds'],10)
                self.assertLess(point['window_seconds'],11)
            finally:
                process.terminate()
                try:process.wait(timeout=6)
                except subprocess.TimeoutExpired:process.kill();process.wait()

    def test_same_sid_legacy_compatibility(self):
        # This established case also passes pristine upstream: no SID/ONID remap.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
                reserve.bind(('127.0.0.1', 0)); port = reserve.getsockname()[1]
            (tmp/'global').write_text('[Global]\nnetwork_id=200\n')
            (tmp/'channels').write_text(f'[Global]\nprovider_name=DVB24\ntransport_stream_id=5311\n[Channel1]\nservice_id=201\nid=legacy\nname=Legacy\neit_mode=1\nsource=udp://127.0.0.1:{port}\n')
            (tmp/'nit').write_text('[Global]\nnetwork_name=DVB24\n[Transponder1]\ntransport_stream_id=5311\nfrequency=0514,0000\nmodulation=128-QAM\nsymbol_rate=006,9000\n')
            (tmp/'epg').write_text('')
            command = [os.environ.get('MPTSD_TEST_BINARY', str(SOURCE/'mptsd')), '-g', str(tmp/'global'),
                '-c', str(tmp/'channels'), '-n', str(tmp/'nit'), '-e', str(tmp/'epg'),
                '-N', '-W', '-f', str(tmp/'out.bin'), '-B', '2', '-q']
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(.2)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    for cc in range(30):
                        pat = section(0, 100, struct.pack('>HH', 201, 0xe064), version=0)
                        pmt = section(2, 201, b'\xe0\x65\xf0\0\x02\xe0\x65\xf0\0', version=0)
                        data = packets(pat, 0)[0] + packets(pmt, 100)[0]
                        data += packets(eit(), cc=cc*2)[0] + packets(eit(table=0x50), cc=cc*2+1)[0]
                        sender.sendto(data, ('127.0.0.1', port)); time.sleep(.02)
                time.sleep(.1)
            finally:
                process.terminate()
                try: process.wait(timeout=6)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            eits = sections_from_ts((tmp/'out.bin').read_bytes(), 18)
            self.assertGreater(len(eits), 10)
            self.assertEqual({e[0] for e in eits}, {0x4e, 0x50})
            for e in eits:
                self.assertEqual(e[3:5], b'\0\xc9')
                self.assertEqual(e[8:12], struct.pack('>HH', 5311, 200))
                self.assertEqual(e[14:-4], eit(table=e[0])[14:-4])

    def test_unreferenced_private_pid_cannot_collide_with_another_pmt(self):
        # A foreign ECM/EMM at input PID192 used to leak onto channel6's output PMT192.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            ports = []
            for _ in range(2):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
                    reserve.bind(('127.0.0.1', 0)); ports.append(reserve.getsockname()[1])
            (tmp/'global').write_text('[Global]\nnetwork_id=200\n')
            channels = '[Global]\nprovider_name=DVB24\ntransport_stream_id=5311\n'
            for index,sid,port in [(1,1001,ports[0]),(6,55,ports[1])]:
                channels += f'[Channel{index}]\nservice_id={sid}\nid=test{index}\nname=Test {index}\neit_mode=1\nsource=udp://127.0.0.1:{port}\n'
            (tmp/'channels').write_text(channels)
            (tmp/'nit').write_text('[Global]\nnetwork_name=DVB24\n[Transponder1]\ntransport_stream_id=5311\nfrequency=0514,0000\nmodulation=128-QAM\nsymbol_rate=006,9000\n')
            (tmp/'epg').write_text('')
            command = [os.environ.get('MPTSD_TEST_BINARY', str(SOURCE/'mptsd')), '-g', str(tmp/'global'),
                '-c', str(tmp/'channels'), '-n', str(tmp/'nit'), '-e', str(tmp/'epg'),
                '-N', '-W', '-f', str(tmp/'out.bin'), '-B', '2', '-m', '1', '-q']
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(.2)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    for cc in range(35):
                        for n,port in enumerate(ports):
                            sid = 201 if n == 0 else 1
                            pat = section(0, 100, struct.pack('>HH', sid, 0xe064), version=0)
                            # PCR/video256 and PMT-declared private-data257 must both survive.
                            pmt = section(2, sid, b'\xe1\x00\xf0\0\x1b\xe1\x00\xf0\0\x06\xe1\x01\xf0\0', version=0)
                            data = packets(pat,0,cc)[0] + packets(pmt,100,cc)[0]
                            pcr = ((90000 + cc * 1800) << 15) | 0x7e00
                            video = bytes([0x47,0x01,0x00,0x30|(cc&15),7,0x10]) + pcr.to_bytes(6,'big')
                            data += video.ljust(188,b'v')
                            private = bytes([0x47,0x01,0x01,0x10|(cc&15)]) + b'DECLARED_PRIVATE_ES'
                            data += private.ljust(188,b'p')
                            # Unreferenced PID192 has a private ECM-style table, not a PMT.
                            foreign = bytes.fromhex('8230284000091fe10000d002001c901a440158842e96a73a3aa15315a1ffe520bfadfc34748706f9759b00')
                            data += packets(foreign,192,cc)[0]
                            data += packets(eit(sid),18,cc)[0]
                            sender.sendto(data,('127.0.0.1',port))
                        time.sleep(.02)
                time.sleep(.1)
            finally:
                process.terminate()
                try: process.wait(timeout=6)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            data = (tmp/'out.bin').read_bytes()
            by_pid = {}
            for i in range(0,len(data)-187,188):
                packet=data[i:i+188];pid=((packet[1]&31)<<8)|packet[2]
                by_pid.setdefault(pid,[]).append(packet)
            pmt_packets=by_pid.get(192,[])
            self.assertGreater(len(pmt_packets),3)
            tables={p[5+p[4]] for p in pmt_packets if p[1]&64 and not p[3]&32}
            self.assertEqual(tables,{2},'foreign private table leaked onto remapped PMT PID')
            pmts=sections_from_ts(data,192)
            self.assertTrue(pmts)
            self.assertEqual({int.from_bytes(p[3:5],'big') for p in pmts},{55})
            for pid in [33,193]:
                self.assertTrue(any(p[3]&32 and p[5]&16 for p in by_pid.get(pid,[])), 'PCR lost')
            for pid in [34,194]:
                self.assertTrue(any(p[4:].startswith(b'DECLARED_PRIVATE_ES') for p in by_pid.get(pid,[])), 'declared private ES lost')
            eits=sections_from_ts(data,18)
            self.assertEqual({int.from_bytes(e[3:5],'big') for e in eits},{1001,55})

    def test_two_inputs_epg_id_mapping_continuity_and_shared_nit(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            ports = []
            for _ in range(2):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                    s.bind(('127.0.0.1', 0)); ports.append(s.getsockname()[1])
            (tmp/'global').write_text(f'[Global]\nnetwork_id=1\noriginal_network_id=99\ntable_version=17\nstatus_file={tmp}/status.json\nstatus_interval_ms=1000\n[Timeouts]\nnit=50\nsdt=50\neit=50\n')
            nit = '[Global]\nnetwork_name=DVB24\n'
            for i in range(4):
                nit += f'[Transponder{i+1}]\ntransport_stream_id={5311+i}\nfrequency=0{514+i*8},0000\nmodulation=128-QAM\nsymbol_rate=006,9000\nservices={1001+i}:1:{i+1}:1\n'
            (tmp/'nit').write_text(nit)
            nits = []
            for tsid in [5311, 5312]:
                (tmp/'status.json').unlink(missing_ok=True)
                channels = f'[Global]\nprovider_name=DVB24\ntransport_stream_id={tsid}\n'
                for i, port in enumerate(ports):
                    channels += f'[Channel{i+1}]\nservice_id={1001+i}\nid=test{i}\nname=Test {i}\neit_mode=1\nsource=udp://127.0.0.1:{port}\n'
                (tmp/'channels').write_text(channels)
                # Input EPG mode must not also synthesize empty p/f from an EPG file.
                (tmp/'epg').write_text('')
                outfile = tmp/f'out{tsid}.bin'
                command = [os.environ.get('MPTSD_TEST_BINARY', str(SOURCE/'mptsd')), '-g', str(tmp/'global'), '-c', str(tmp/'channels'),
                           '-n', str(tmp/'nit'), '-e', str(tmp/'epg'), '-N', '-W', '-f', str(outfile),
                           '-B', '2', '-m', '0', '-p', '0', '-9', '-q']
                with (tmp/'log').open('wb') as log:
                    process = subprocess.Popen(command, stdout=log, stderr=log)
                    try:
                        time.sleep(.2)
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                            cc = [0, 0]
                            deadline = time.monotonic() + 1.2
                            while time.monotonic() < deadline:
                                for i, port in enumerate(ports):
                                    sid = 201+i
                                    pat = section(0, 100, struct.pack('>HH', sid, 0xe064), version=0)
                                    pmt = section(2, sid, b'\xe0\x65\xf0\0\x02\xe0\x65\xf0\0', version=0)
                                    data = packets(pat, 0)[0] + packets(pmt, 100)[0]
                                    for table in [0x4e, 0x50]:
                                        ep = packets(eit(sid, table, 400), cc=cc[i]); cc[i] += len(ep)
                                        data += b''.join(ep)
                                    # UDP inputs accept MPEG-TS aligned datagrams <=1316 bytes.
                                    for off in range(0, len(data), 1316):
                                        sender.sendto(data[off:off+1316], ('127.0.0.1', port))
                                time.sleep(.02)
                        time.sleep(.1)
                        self.assertIsNone(process.poll(), (tmp/'log').read_text())
                    finally:
                        process.terminate()
                        try: process.wait(timeout=6)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                data = outfile.read_bytes()
                status=json.loads((tmp/'status.json').read_text())
                self.assertEqual(status['pid'],process.pid)
                self.assertEqual(status['sequence'],1)
                self.assertGreaterEqual(status['window_seconds'],1)
                self.assertEqual(status['total_packets'],status['window_total_packets'])
                self.assertLessEqual(status['total_packets']*188,len(data))
                sent=data[:status['total_packets']*188]
                null_count=sum((((sent[i+1]&31)<<8)|sent[i+2])==8191 for i in range(0,len(sent),188))
                self.assertEqual(status['null_packets'],null_count)
                self.assertEqual(status['write_errors'],0)
                self.assertGreater(status['non_null_mbps'],0)
                self.assertLess(status['occupancy_pct'],100)
                self.assertAlmostEqual(status['output_mbps'],status['non_null_mbps']+status['null_mbps'],places=5)
                eits = sections_from_ts(data, 18)
                self.assertGreater(len(eits), 20, (tmp/'log').read_text())
                self.assertEqual({(e[0], int.from_bytes(e[3:5], 'big')) for e in eits},
                                 {(t, sid) for t in [0x4e, 0x50] for sid in [1001,1002]})
                for e in eits:
                    sid = int.from_bytes(e[3:5], 'big')
                    self.assertEqual(e[8:12], struct.pack('>HH', tsid, 99))
                    self.assertEqual(e[14:-4], eit(sid-800, e[0], 400)[14:-4])
                pats = sections_from_ts(data, 0)
                self.assertEqual((pats[0][5] >> 1) & 31, 17)
                sdts = sections_from_ts(data, 17)
                self.assertEqual((sdts[0][5] >> 1) & 31, 17)
                self.assertEqual(sdts[0][8:10], b'\0\x63')
                self.assertEqual(sdts[0][13] & 3, 3)  # p/f and schedule advertised
                nits.append(sections_from_ts(data, 16)[0])
                self.assertEqual((nits[-1][5] >> 1) & 31, 17)
            self.assertEqual(nits[0], nits[1], 'all muxes in network need identical NIT')

if __name__ == '__main__': unittest.main()
