# Changes from upstream

## Standalone fork — 2026-11-16

Based on `gfto/mptsd` revision `a2b6e4d057e1f68c6a1f9d1b9c80732fa4ef9198`.
Original Git history, copyright notices and licenses are retained. The changes
were developed in the headend project; see [provenance](docs/PROVENANCE.md).

### EPG and service/network metadata

- Add `eit_forward.c/h`: reassemble and forward input actual-TS present/following
  and schedule EIT, mapping SID/TSID/ONID while preserving event bytes and versions.
- Handle full-size sections, pointer fields, packed sections, duplicates and
  continuity loss; enqueue complete sections atomically and assign output EIT CC.
- Skip file-generated EIT for input-EIT channels; advertise schedule EIT in SDT.
- Add explicit `original_network_id` and `table_version` global configuration.
- Add `services=SID:TYPE:LCN:VISIBLE,...` per NIT transport, avoid duplicate local
  transport entries and validate single-section NIT/SDT capacity.
- Encode configured non-ASCII SI names with the DVB UTF-8 selector. Input EPG
  text and its original encoding selectors are never converted.

### CAS and PID handling

- Filter unreferenced source PIDs; retain declared ES/PCR and CA PIDs. This fixes
  a private source PID leaking onto a different output program's PMT.
- Add `cas_remux.c/h`: discover ECM from PMT and EMM from complete CAT versions,
  reserve independent CAS ranges, rewrite CA descriptor PID references, merge CAT
  and derive SDT CA indication from PMT.
- Reject incompatible PID roles, malformed/future/incomplete tables and resource
  exhaustion. Update advertisement/filter state on valid PMT/CAT changes and
  input disconnects; retain bounded sticky PID reservations while packets queue.
- Replace the temporary single-channel PID 2098 exception with general discovery.
- Add explicitly identified shared EMM groups with one output PID and continuous
  section packetization; suppress duplicates and switch source at section boundaries.
- Add optional global UDP input of raw EMM sections, independent of the channels,
  including bounded datagram validation and no stale cached-entitlement replay.

### Telemetry and robustness

- Add `output_status.c/h`: successful-write NULL/non-NULL counters and atomic
  JSON occupancy statistics, normally once per ten seconds, without extra receivers.
- Make circular-buffer section insertion atomic across wrap and overflow.
- Correct a scoped `struct tm` lifetime issue and use unsigned CRC initialization
  arithmetic to avoid undefined behavior without changing generated CRC bytes.
- Make multicast loopback explicit, accept TTL 0 and normalize PCR modes to 0..3.
  The PCR algorithms themselves remain upstream behavior.

### Standalone packaging

- Include the pinned libfuncs/libtsfuncs sources and scoped library changes;
  preserve each library's MIT license instead of requiring submodule downloads.
- Add independent tests, portable example configuration, `make test` and
  `make install` with PREFIX/DESTDIR. Correct the Makefile's CC assignment syntax.
- Keep the ordinary CLI executable separate from web management and deployment.
