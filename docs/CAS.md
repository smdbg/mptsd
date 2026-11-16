# ECM/EMM remultiplexing and shared EMM

## Automatic discovery

No manual PID list or channel-specific exception is required for ordinary
scrambled SPTS inputs. Program-level and ES-level PMT CA descriptors (tag 0x09)
supply ECM PIDs; complete current CATs supply EMM PIDs.

Each input has a separate 128-PID CAS range in `0x1000..0x1f7f` (4096..8063).
These do not overlap the bounded 32-PID-per-channel PMT/PCR/ES ranges. References
in PMT/CAT are rewritten to the new PIDs. A combined output CAT is emitted on
PID 1. The CAID, CA-private bytes and opaque ECM/EMM payloads remain unchanged.
SDT `free_CA_mode` follows the PMT CA signaling.

Independent entitlement feeds are not merged merely because their CAIDs match.
Only CA descriptors are combined from CAT; unrelated private descriptor context
is not copied between sources. The mux does not decrypt or create entitlements.

## Explicit shared group from channel inputs

For channels known to carry the same EMM source, add this to each participating
`[ChannelN]` in the channels file:

```ini
emm_shared_id = example-cas
emm_shared_caid = 0x0B00
emm_shared_pid = 90
```

The identity must be 1..64 ASCII letters/digits or `_`, `-`, `.`. Set all three
values together. CAID must be 1..65535; PID is the input EMM PID, 32..8190.
The same identity cannot have different CAIDs. Only the declared CAID/PID from
CAT participates; ECM and other EMM PIDs retain independent mappings.

Each shared identity receives one output PID in 8064..8095 and one matching CAT
descriptor. CA-private bytes must agree; conflicting input descriptors are
logged and that input is not used for shared EMM. Per-input queues hold at most
16 complete sections. One member supplies EMM; duplicates from others are
suppressed. After two seconds without EMM, another fresh complete section can
take over, with continuous output CC and no half-section switch. Old cached
rights are not repeatedly emitted when all inputs stop.

This affects only the remultiplexed output. Original standalone channel outputs
can still carry their own EMM and CAT.

## Global raw-section EMM input

To supply EMM independently of which channels are present, add this to `[Global]`
in the global `-g` configuration:

```ini
emm_shared_id = example-cas
emm_shared_caid = 0x0B00
emm_shared_group = 239.255.10.1
emm_shared_port = 41998
emm_shared_interface = 127.0.0.1
```

The group, port and interface above are examples. An external sender must send
fresh EMM sections to that multicast group on the selected interface. The
sender/generator is not bundled with mptsd. This UDP input carries **raw sections,
not MPEG-TS packets, RTP or SimulCrypt messages**.

A datagram contains one or more concatenated complete sections: table_id in
0x82..0x8f, two section-length bytes, then the payload specified by that length.
Sections may be up to 4096 bytes including their three-byte header. Limits are
8192 bytes and 32 sections per datagram. The whole datagram is validated before
any part is scheduled. No partial section may span two datagrams.

mptsd packetizes these sections once on output PID 8064 with a CAT CA descriptor.
It can carry this feed even when all selected channels are clear. Mark channels
that also contain the same feed with the matching shared identity/CAID and input
PID described above; their copies are suppressed while this global source is
configured. There is no replay of stale cached rights and no fallback to those
suppressed channel copies when the configured global source stops.

## Input changes and bounds

- Invalid, incomplete or future PMT/CAT versions do not replace a coherent state.
  A compatible candidate pair can be held for up to ten seconds while related
  PMT and CAT changes arrive; repeated candidates refresh the wait.
- Complete current tables update the advertised/filter state; input disconnects
  withdraw that input's CAS contribution. Packets already queued may still drain.
- Per-input CAT descriptors are limited to 4096 bytes. Combined CAT sections
  are emitted gradually rather than overflowing the PSI queue.
- Sticky CAS reservations prevent reuse while old packets might remain queued.
  An input can reserve at most 128 distinct CAS PIDs over its lifetime. If
  repeated changes exhaust that range, a new table is rejected and logged;
  restarting that mux frees the reservations.
- Role collisions, inconsistent private bytes and capacity errors are explicit;
  a conflicting new table does not silently corrupt another service's mapping.
- Private CAS protocols embedding their own PID references inside opaque ECM/EMM
  data or private descriptors need separate support. These bytes are not rewritten.

Tests use synthetic private sections, never actual keys. CAS signaling and EMM
transport alone do not establish whether a particular receiver/card has valid
rights; that requires a receiver test with the intended system.
