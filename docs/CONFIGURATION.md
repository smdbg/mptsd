# Standalone configuration

Use one process per output transport, with the four `-g`, `-c`, `-n`, `-e`
configuration files. The complete example is in [../examples](../examples).
File names can be different for each process; no headend manager is required.

## Global settings (`-g`)

```ini
[Global]
network_id = 1
original_network_id = 1
table_version = 0
status_file = /dev/shm/mptsd-tp1.json
status_interval_ms = 10000
```

`original_network_id` defaults to `network_id`. `table_version` seeds generated
PAT/NIT/SDT and offsets incoming PMT versions modulo 32. Increment it modulo 32
when changing advertised service/network data. It does not replace input EIT
versions. Use the same version across a coordinated network configuration.

The optional status filename must be absolute and its parent writable by the
process. Publishing is disabled if omitted. The interval range is 1000..3600000
ms, default 10000. Each process needs a different filename. Counters include:

- `pid`, `sequence`, `sample_unix_ms`, `window_seconds`;
- cumulative and window `total_packets` / `null_packets`;
- `output_mbps`, `non_null_mbps`, `null_mbps`, `occupancy_pct`, `write_errors`.

Only complete successful output writes count. RTP headers are excluded.
File-only `-N -W` output counts complete file writes. No output gives null
occupancy rather than a fabricated zero; failed status publication retains the
window for a later retry. Atomic replacement uses mode 0600. Check PID, sequence
and timestamp before using an old status file after a restart.

The inherited `[Timeouts]` section controls table repetition; see the original
`mptsd.conf` sample. It is independent of `status_interval_ms`.

## Channel settings (`-c`)

```ini
[Global]
provider_name = Example Provider
transport_stream_id = 1

[Channel1]
id = channel-1
name = Channel 1
service_id = 101
radio = no
lcn = 1
lcn_visible = yes
eit_mode = 1
source1 = udp://239.10.0.1:1234
# source2 = http://192.0.2.10:8080/channel1.ts
```

Channel sections are numbered `Channel1` through `Channel31`. Each input must
be an SPTS. `service_id` is the **output** SID; the input PAT supplies its original
SID. Output IDs must be unique. `source` is an alias for `source1`; up to seven
numbered sources provide inherited failover behavior. Use raw UDP, MPEG-TS RTP
or HTTP transport-stream sources, not HLS playlists. Each channel needs a unique
identifier and the LCN appropriate to your network plan.

For `eit_mode=1`, actual-TS p/f (table 0x4e) and schedule (0x50..0x5f) EIT for
the input PAT service pass through with mapped IDs and a corrected CRC.
Event descriptors, original text/encoding, language codes and versions remain
unchanged. Other-TS EIT is not relabelled as actual-TS. File-generated EIT is
skipped for these channels. An empty `-e` file is therefore enough for the
example. `eit_mode=0` retains the inherited file-EPG behavior.

Configured non-ASCII service/provider/network names use DVB UTF-8 signaling;
ASCII stays ASCII. This fork has no selectable name codepage. That setting is
separate from the unchanged character encodings inside input EPG.

## NIT settings (`-n`)

Every mux in one network should receive the same complete file, for example:

```ini
[Global]
network_name = Example DVB-C

[Transponder1]
transport_stream_id = 1
frequency = 0514,0000
modulation = 64-QAM
symbol_rate = 006,9000
services = 101:1:1:1,102:1:2:1

[Transponder2]
transport_stream_id = 2
frequency = 0522,0000
modulation = 64-QAM
symbol_rate = 006,9000
services = 201:1:3:1,202:1:4:1
```

Each `services` entry is `SID:TYPE:LCN:VISIBLE`, with decimal numbers.
Here type 1 means digital television and visible 1 means visible in the LCN
list. This lets all outputs advertise other transports' service lists and LCNs,
not only the local transport. Keep the entries consistent with each channel
configuration and enable LCN on every process with `-9`.

The fixed-width frequency and symbol-rate notation above is inherited from
upstream: `0514,0000` means 514 MHz and `006,9000` means 6.9 Msymbol/s. Supported
QAM strings are `16-QAM`, `32-QAM`, `64-QAM`, `128-QAM`, `256-QAM`. These values
populate cable delivery descriptors; they do not configure RF hardware.

There may be up to 31 transports and 31 listed services per transport, subject
to the single-section NIT byte limit. NIT and SDT are validated against their
1024-byte section budgets. Use consistent network IDs/ONID and unique TSIDs.
The current LCN format is NorDig PDS 0x29 + descriptor 0x83, not selectable.

## Output and operation

- `-O ADDRESS -P PORT`: UDP destination. Use a port from 1024 to 65535.
- `-o LOCAL_IPV4`: output interface address, not an interface name.
- `-t TTL`: multicast TTL; 0 is supported for host-local traffic.
- `-s SSRC`: nonzero SSRC enables RTP output instead of raw UDP.
- `-B MBPS`: CBR target from 2 to 75 Mbps; includes NULL packets and signaling.
- `-m 0/1/2/3`: preserve/move/rewrite/move-and-rewrite PCR respectively.
- `-9`: enable LCN (disabled by default).
- `-N -W -f capture.ts`: disable network and record the output locally.

For a bounded example recording, with the configured sources already active:

```bash
timeout --signal=TERM 10s mptsd \
  -g examples/mptsd.conf -c examples/mptsd_channels.conf \
  -n examples/mptsd_nit.conf -e examples/mptsd_epg.conf \
  -N -W -f capture.ts -B 38 -m 3 -9
```

`timeout` normally returns 124 at its deadline. Native periodic configuration
checks exist, but for coordinated changes across multiple outputs, stop/restart
the affected processes with consistent files and versions. No cross-process
transaction manager is included. Use distinct output destinations and status
paths. The program does not transcode to make an overloaded output fit.
