# mptsd — standalone fork

A fork of [gfto/mptsd](https://github.com/gfto/mptsd), originally by Georgi
Chorbadzhiyski. It combines single-program MPEG-TS inputs into a constant-bitrate
multiprogram transport, typically for a DVB-C modulator. One process produces
one output transport. The original Git history and author notices are preserved.

This fork adds input EPG identifier remapping, automatic ECM/EMM remultiplexing,
optional shared EMM, network-wide NIT service/LCN descriptions and native output
occupancy counters. It is a standalone C program: no Docker, headend manager,
FFmpeg or transcoding is required.

## Build and install

On Debian / Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y git build-essential python3
git clone https://github.com/smdbg/mptsd.git
cd mptsd
make -j2
make test
sudo make install
```

The supported/tested platform for this fork is Linux. Python 3 is needed only
for the optional tests. The runtime uses system C, pthread, math and realtime
libraries. `libfuncs` and `libtsfuncs` are bundled at pinned revisions, including
our scoped fixes; **no submodule downloads or external media frameworks** are
needed. Linux high-resolution timers must be available for output pacing.

Install defaults to `/usr/local`. `PREFIX` and `DESTDIR` are supported; for
example `make install PREFIX="$HOME/.local"`. Run `./mptsd` if not installed.
For an explicit reproducible version identity:

```bash
make clean
make -j2 REPRODUCIBLE_BUILD=1 GIT_VER=YOUR_SOURCE_REVISION
```

## Two-channel example

The [examples](examples) directory contains a complete, synthetic configuration:

| File | Purpose |
| --- | --- |
| `mptsd.conf` | Network ID, ONID, table version and optional status output |
| `mptsd_channels.conf` | Two UDP SPTS inputs, output SIDs, names, LCNs and input EPG |
| `mptsd_nit.conf` | DVB-C delivery metadata and service/LCN list |
| `mptsd_epg.conf` | Empty: the example uses input EPG instead of file-generated EIT |

Edit copies of these files to match your sources. The inputs in the example are
`239.10.0.1:1234` and `239.10.0.2:1234`. The output below is `239.20.0.1:1234`:

```bash
mptsd -i example-tp1 \
  -g examples/mptsd.conf -c examples/mptsd_channels.conf \
  -n examples/mptsd_nit.conf -e examples/mptsd_epg.conf \
  -O 239.20.0.1 -P 1234 -o 0.0.0.0 -t 1 -B 38 -m 3 -9
```

`-o` is the output interface's local IPv4 address; `0.0.0.0` uses the route.
Input multicast membership also needs the appropriate host routing. Use your
actual interface address when the host has multiple networks. Avoid feeding the
output back into an input.

`-B` is the total output bitrate in **Mbps**, including NULL packets and tables;
select it for your modulator. Frequency, QAM and symbol rate in the NIT describe
the RF network to receivers: they do not program a hardware modulator. The
example advertises 514 MHz / 64-QAM / 6900 kSym/s and outputs approximately
38 Mbps. The native writer rounds its target to complete 1316-byte datagrams.

`-9` enables LCN. The current private-data specifier is **NorDig `0x00000029`**,
followed by descriptor `0x83`; receiver behavior depends on its tuning profile.
There is no selectable EACEM profile in this fork yet.

Run in the foreground and stop with Ctrl+C. Each additional output transport
needs its own process, channel configuration and status-file path. Give all
processes in one network the same complete NIT configuration and consistent
network/table versions. See [configuration details](docs/CONFIGURATION.md).

## EPG passthrough

Set `eit_mode=1` on an input channel. Actual-transport EIT present/following and
schedule sections are retained. Output SID/TSID/ONID and CRC are updated while
event descriptions, languages, character-set selectors and input EIT versions
are preserved. File-generated EIT is skipped for that channel.

The original same-SID case remains covered by tests; the new behavior also
handles an output SID different from the incoming PAT service. This is not an
EPG text conversion. Input EPG must exist in the source; the mux cannot recreate
missing schedule data. Output TDT/TOT is generated from the host clock.

## Scrambled channels and shared EMM

ECM PIDs are discovered automatically from program- and ES-level PMT CA
descriptors. EMM PIDs come from complete current CATs. The mux allocates separate
output PIDs, rewrites the descriptors and produces a combined CAT. CAID,
private descriptor bytes and ECM/EMM payload contents are preserved.

No PID exception for a particular channel is required. Invalid tables and
conflicting PID roles are rejected. Unknown source PIDs are filtered instead of
leaking onto another output service's PID. This is remultiplexing of already
scrambled content; the program does not encrypt or decrypt it.

For several channels known to share one entitlement source, optional shared-EMM
settings produce a single output EMM PID. A global raw-section UDP input can
also supply EMM independently of the selected channels. This feature needs an
external compatible EMM sender; the fork does not include an EMM generator.
See [CAS configuration and limits](docs/CAS.md).

## Occupancy statistics

The example enables:

```ini
[Global]
status_file = /dev/shm/mptsd-example-status.json
status_interval_ms = 10000
```

Read the JSON file with:

```bash
python3 -m json.tool /dev/shm/mptsd-example-status.json
```

`occupancy_pct` is the non-NULL fraction of successfully emitted TS packets,
including audio/video, EPG and other signaling. `output_mbps`, `non_null_mbps`,
`null_mbps`, packet counts and `write_errors` are also available. The existing
writer counts packets; no extra UDP receiver or stream scanner is started.
Snapshots are atomically replaced, mode 0600. No measurement is invented before
the first complete window. UDP write success does not prove receiver delivery.

Omit `status_file` to disable publishing. Use a distinct absolute filename for
each process; `/dev/shm` keeps these small snapshots in RAM.

## PCR modes and limits

| `-m` | Existing PCR behavior |
| --- | --- |
| 0 | Preserve PCR |
| 1 | Move PCR to its calculated output position |
| 2 | Rewrite PCR using output bitrate |
| 3 | Move and rewrite PCR |

The PCR algorithms remain upstream behavior. The program does not change codec
bitrates or transcode streams. Keep total input payload below output capacity.
This is a **CBR mux with NULL stuffing**, unlike the separate
[dvbt-mux](https://github.com/smdbg/dvbt-mux) VBR program.

Limits include 31 channels per process, seven failover sources per channel and
31 NIT transports. Inputs must be SPTS; use a separate service selector for an
MPTS source. Generated NIT and SDT each fit one 1024-byte section; encoded table
size, not only service count, limits the configuration. Private CAS protocols
that embed PID references inside opaque ECM/EMM data need separate support.

## Tests and attribution

`make test` runs 14 protocol/buffer/status tests and 23 CAS tests using local
synthetic MPEG-TS sources. No DVB hardware, receiver, card, CAM server or actual
keys are needed. The tests cover EPG byte preservation, identifier remapping,
NIT/SDT, PID collisions, shared EMM, CAT lifecycle and output counters.

For the EIT parser's sanitizer harness:

```bash
cc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  -I . tests/mptsd_eit_sanitize.c eit_forward.c -o /tmp/mptsd-eit-test
/tmp/mptsd-eit-test
```

See [CHANGELOG.md](CHANGELOG.md), [source provenance](docs/PROVENANCE.md),
[COPYING](COPYING) and the original [README](README). The main program is GPL v2;
the bundled helper libraries retain their MIT licenses. `mptsd -h` lists the
inherited command-line options, including RTP, file output and daemon mode.
