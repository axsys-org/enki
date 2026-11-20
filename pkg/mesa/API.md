# Mesa syscall reference

All operations belong to opset 83 and take exactly two arguments: a row of
wormholes and a normal data row. Prefix every operation below with `Mesa`.
The reply is `[status, data, new_handles]`; status 0 is success, status 1 carries
error text in `data` and an empty handle row. `→` lists the new handles in order;
where no data is specified it is 0. `—` means an empty input handle row.
`!` consumes the native object, invalidating every alias of that wormhole.
Consumption can happen before a native constructor returns an error; do not
retry using consumed inputs. Other input handles are borrowed for the call.

Rows are PLAN applications headed by 0; the empty row is nat 0. Bytes and text
are **bars**: bytes followed by a terminal byte 1, interpreted as a little-endian
nat. This preserves trailing zeroes. Operation names themselves are ordinary
cord nats, without this sentinel. Booleans are 0/1. An optional value is 0 or a
one-element row. All indices are zero-based. Scalar overflow is an error.

## Operations

| Operation (Mesa prefix) | Input handles, in order | Data argument | Successful output |
| --- | --- | --- | --- |
| `ActionsCount` | Actions | [] | remaining count |
| `ActionsNext` | Actions | [] | next action, or 0 when exhausted; may → CommitRequest |
| `Agree` | KeyPair, PublicKey | [] | → SharedSecret |
| `Atom` | — | [bytes] | → Atom (canonical little endian) |
| `AtomBytes` | Atom | [] | bytes |
| `AtomSize` | Atom | [] | byte count |
| `Bits` | — | [bytes, bit_length] | → Bits |
| `BitsAtom` | Bits | [max_bytes] | → Atom |
| `BitsBit` | Bits | [index] | optional boolean |
| `BitsFromAtom` | Atom | [] | → Bits |
| `BitsSize` | Bits | [] | bit count |
| `BitsSlice` | Bits | [start, length] | → Bits (copy) |
| `BoundKeys` | BoundPass | [] | → PublicKey name_key, PublicKey live_key |
| `BoundName` | BoundPass | [] | 128-bit name |
| `BoundRequireName` | BoundPass | [name] | 0, or error |
| `Buffer` | — | [bytes] | → Buffer |
| `BufferBytes` | Buffer | [] | bytes |
| `BufferSlice` | Buffer | [start, length] | → Buffer (copy) |
| `CheckRing` | BoundPass, Ring | [] | 0, or error |
| `Checkpoint` | Peer local, Flows | [batch_size] | → Checkpoint (empty) |
| `CheckpointClone` | Checkpoint | [] | → Checkpoint |
| `CheckpointExport` | Checkpoint | [] | JSON bytes |
| `CheckpointFlow` | Checkpoint | [flow_id] | 0 absent or [flow_JSON, next_receive, batch_settled, valid] |
| `CheckpointImport` | — | [JSON bytes] | → Checkpoint (validated by Node/Store when used) |
| `CheckpointRevision` | Checkpoint | [] | optional expected revision |
| `Close` | Any! | [] | 0 |
| `CommitFields` | CommitRequest | [] | [base_revision, change]; → CommitToken, Checkpoint |
| `Config` | — | core_config or [] | → Config |
| `ConfigFields` | Config | [] | core_config |
| `Cue` | Bits | noun_limits | → Noun |
| `Domain` | — | [domain_text] | validated domain text |
| `Engine` | Config, Peer local, Protection!, optional Policy | [] | → Engine |
| `EngineAdvanceTo` | Engine | [tick] | → Actions |
| `EngineBind` | Engine | [path, bytes, optional_allowed_peer] | → Actions |
| `EngineCancel` | Engine | [request_id] | → Actions |
| `EngineInstallPeer` | Engine, Peer | [] | → Actions |
| `EnginePacketLimit` | Engine | [] | bytes |
| `EngineRead` | Engine | [request_id, peer_id, path] | → Actions |
| `EngineReceive` | Engine | [claimed_peer, attachment, packet_bytes] | → Actions |
| `EngineRemoteBreach` | Engine, Peer | [] | → Actions |
| `EngineRetireBefore` | Engine | [prefix, suffix] | → Actions |
| `EngineSetAttachment` | Engine | [peer_id, optional_attachment] | → Actions |
| `EngineSetService` | Engine | [peer_id, allowed] | → Actions |
| `EngineTransferState` | Engine | [peer_id] | 0 absent or [window, in_flight, queued] |
| `EngineUnbind` | Engine | [path] | → Actions |
| `EngineUsage` | Engine | [] | core_usage |
| `ExpectsPair` | — | [leaves, index] | boolean |
| `FlowConfig` | Config, Policy | [] or [max_flows, max_messages, window, max_message_bytes, max_outbox_bytes] | → FlowConfig |
| `FlowConfigFields` | FlowConfig | [] | five fields above; → Config, Policy |
| `Flows` | Peer... | \[[flow_id, optional_attachment]...\] | → Flows |
| `FlowsFields` | Flows | [] | flow/attachment rows; → Peer per flow |
| `Host` | Node!, Lane! | [peer_id, attachment] | notices; → Host, Actions (process initial batch) |
| `HostAdvanceTo` | Host | [tick] | notices; → Actions |
| `HostCheckpoint` | Host | [] | → Checkpoint |
| `HostCommitted` | Host, CommitToken | [] | notices; → Actions |
| `HostConnected` | Host | [] | boolean |
| `HostCoreUsage` | Host | [] | core_usage |
| `HostDecide` | Host | [message_id, outcome] | notices; → Actions |
| `HostDisconnect` | Host | [] | notices; → Actions |
| `HostEnqueue` | Host | [flow_id, sequence, bytes] | notices; → Actions |
| `HostHalted` | Host | [] | boolean |
| `HostInvalidateEpoch` | Host | [peer_id, new_rift] | notices; → Actions |
| `HostNodeUsage` | Host | [] | flow_usage |
| `HostPacketLimit` | Host | [] | bytes |
| `HostPoll` | Host | [tick] | notices; → Actions |
| `HostReconnect` | Host, Lane! | [] | notices; → Actions |
| `HostResume` | Host | [flow_id] | notices; → Actions |
| `HostSetService` | Host | [peer_id, allowed] | notices; → Actions |
| `HostUpdatePeer` | Host, Peer | [] | notices; → Actions |
| `IsMesa` | — | [bytes] | boolean |
| `Jam` | Noun | noun_limits | → Bits |
| `KeyFromSeed` | — | [32-byte seed] | → KeyPair |
| `KeyPublic` | KeyPair | [] | → PublicKey |
| `Kind` | Any | [] | kind text |
| `LaneAddress` | UDP Lane | [] | local address text |
| `LaneBuffers` | TCP Lane | [] | [queued_bytes, buffered_bytes] |
| `LaneCapabilities` | Lane | [] | [max_packet, reliable, ordered, framing] (0 datagram / 1 length-prefixed) |
| `LaneFlush` | Lane | [] | 0 |
| `LaneReceive` | Lane | [] | [0] idle, [1] discarded, [2, bytes] packet |
| `LaneSend` | Lane | [packet_bytes] | accepted boolean (false means retry) |
| `Mat` | Atom | [max_atom_bits] | → Bits |
| `MatDecode` | Bits | [max_atom_bits] | consumed bits; → Bits |
| `MessagePath` | — | [message_id, read_kind] | path bytes |
| `Mug` | — | [bytes] | mug |
| `Node` | FlowConfig, Peer local, Protection!, Flows, Checkpoint | [incarnation] | → Node |
| `NodeAdvanceTo` | Node | [tick] | → Actions |
| `NodeCheckpoint` | Node | [] | → Checkpoint (copy) |
| `NodeCommitted` | Node, CommitToken | [] | → Actions |
| `NodeCoreUsage` | Node | [] | core_usage |
| `NodeDecide` | Node | [message_id, outcome] | → Actions |
| `NodeEnqueue` | Node | [flow_id, sequence, bytes] | → Actions |
| `NodeHalted` | Node | [] | boolean |
| `NodeInvalidateEpoch` | Node | [peer_id, new_rift] | → Actions |
| `NodePacketLimit` | Node | [] | bytes |
| `NodeReceive` | Node | [claimed_peer, attachment, packet_bytes] | → Actions |
| `NodeResume` | Node | [flow_id] | → Actions |
| `NodeSetAttachment` | Node | [peer_id, optional_attachment] | → Actions |
| `NodeSetService` | Node | [peer_id, allowed] | → Actions |
| `NodeUpdatePeer` | Node, Peer | [] | → Actions |
| `NodeUsage` | Node | [] | flow_usage |
| `NounCount` | Noun | [] | node index / node count |
| `NounNode` | Noun | [index] | 0 absent, [2, head, tail] cell, or 1 atom; → Bits for atom |
| `NounRoot` | Noun | [] | node index / node count |
| `Packet` | — | [hop, body, wire_limits] | → Packet v2 |
| `PacketFields` | Packet | [] | [hop, body] (version-specific name fields) |
| `PacketV1` | — | [hop, body, wire_limits, checksum_mode] | → Packet v1 |
| `Pass` | — | [bytes, domain_text, identity_limits] | → Pass / Ring (owned backing) |
| `PassBind` | Pass | [] | → BoundPass |
| `PassClaims` | Pass, BoundPass or Ring | [] | domain text; → Bits dat, Bits domain_tail, Bits evidence |
| `PassKeys` | Pass or BoundPass | [] | [genesis_key, live_key] |
| `Peer` | — | peer_fields | → Peer |
| `PeerClone` | Peer | [] | → Peer |
| `PeerFields` | Peer | [] | peer_fields |
| `Policy` | — | transfer_policy | → Policy |
| `PolicyFields` | Policy | [] | transfer_policy |
| `PolicyFixed` | Config | [] | → Policy |
| `ProofHashes` | — | [total_bytes] | hash count |
| `Protection` | KeyPair!, PublicKey... | \[local_key_id, [remote_key_id...]\] | → Protection (SignedPublic) |
| `ProtectionLocalKey` | Protection, Peer | [] | boolean |
| `ProtectionPeerKey` | Protection, Peer | [] | boolean |
| `ProtectionSignRoot` | Protection, Peer | [name_v2, total_bytes, root_hash] | optional signature |
| `ProtectionVerify` | Protection, Peer local, Peer remote | [path] | boolean |
| `ProtectionVerifyRoot` | Protection, Peer | [name_v2, total_bytes, root_hash, signature] | boolean |
| `PublicAddScalar` | PublicKey | [32-byte scalar] | → PublicKey |
| `PublicBytes` | PublicKey | [] | 32-byte key |
| `PublicClone` | PublicKey | [] | → PublicKey |
| `PublicKey` | — | [32-byte key] | → PublicKey |
| `ReconstructFragment` | — | [total_bytes, bloq, index, bytes, max_bytes] | fragment bytes |
| `Relay` | — | [peer_a, peer_b, max_packet_a, max_packet_b, wire_limits, max_interests, interest_ttl] | → Relay |
| `RelayAdvanceTo` | Relay | [tick] | 0 |
| `RelayDisconnect` | Relay | [leg] | 0 |
| `RelayPending` | Relay | [] | pending count |
| `RelayReceive` | Relay | [leg, bytes] | [0, drop_reason] or [1, outgoing_leg, bytes] |
| `RelayTimer` | Relay | [] | optional tick |
| `RetryDelay` | Policy | [attempt] | ticks |
| `Ring` | — | [bytes, domain_text, identity_limits] | → Pass / Ring (owned backing) |
| `RingKeys` | Ring | [] | [genesis_key, live_seed] (explicit secret export) |
| `RootContext` | Peer | [name_v2, total_bytes, root_hash] | bytes |
| `Rub` | Bits | [max_atom_bits] | consumed bits; → Bits |
| `SecretBytes` | SharedSecret | [] | shared secret bytes (explicit export) |
| `Sha256` | — | [bytes] | 32-byte hash |
| `SiftHead` | — | [bytes] | 0 absent or [nex, typ, hop, gum] |
| `SiftV1Head` | — | [bytes] | 0 absent or [nex, typ, hop, gum] |
| `Sign` | KeyPair | [message] | 64-byte signature |
| `SnapshotCheckpoint` | Snapshot | [] | → Checkpoint |
| `SnapshotCounts` | Snapshot | [] | [deliveries, settlements] |
| `SnapshotDelivery` | Snapshot | [index] | [revision, message_id, bytes, outcome] |
| `SnapshotSettlement` | Snapshot | [index] | [revision, message_id, outcome] |
| `StoreCheckpoint` | Store | [] | → Checkpoint |
| `StoreCommit` | Store, CommitRequest | [] | → CommitToken after durable commit |
| `StoreCreate` | Checkpoint initial_scope | [path_text, store_limits] | → Store (open recovers current state and fences older writers) |
| `StoreIncarnation` | Store | [] | 128-bit incarnation |
| `StoreInspect` | — | [path_text, store_limits] | → Snapshot (read only) |
| `StoreOpen` | Checkpoint initial_scope | [path_text, store_limits] | → Store (open recovers current state and fences older writers) |
| `StorePrepare` | Store, CommitRequest | [] | → Transaction |
| `TcpAccept` | TcpListener | [] | 0 when idle; otherwise address text and → TcpSocket |
| `TcpConnect` | — | [numeric_socket_address, timeout_ms] | → TcpSocket (timeout 1..60000) |
| `TcpLane` | TcpSocket! | [max_packet, queue_bytes] | → Lane |
| `TcpListen` | — | [numeric_socket_address] | → TcpListener |
| `TcpListenAddress` | TcpListener | [] | address text |
| `Token` | — | [incarnation, revision] | → CommitToken |
| `TokenFields` | CommitToken | [] | [incarnation, revision] |
| `TransactionCommit` | Transaction! | [] | → CommitToken after durable commit |
| `TransactionRollback` | Transaction! | [] | 0 |
| `Tree` | — | [bytes] | → Tree |
| `TreePair` | Tree | [index] | 0 absent or [hash, hash] |
| `TreeProof` | Tree | [] | proof bytes |
| `TreeRoot` | Tree | [] | 32-byte hash |
| `TreeSize` | Tree | [] | storage bytes |
| `UdpAddress` | UdpSocket | [] | address text |
| `UdpBind` | — | [numeric_socket_address] | → UdpSocket |
| `UdpHost` | Engine!, UdpSocket! | \[[peer_id, attachment, numeric_address]...\] | → UdpHost |
| `UdpHostAddress` | UdpHost | [] | address text |
| `UdpHostAdvanceTo` | UdpHost | [tick] | → Actions |
| `UdpHostBind` | UdpHost | [path, bytes] | → Actions (public access) |
| `UdpHostCancel` | UdpHost | [request_id] | → Actions |
| `UdpHostDiscarded` | UdpHost | [] | discarded datagram count |
| `UdpHostInstallPeer` | UdpHost, Peer | [] | → Actions |
| `UdpHostPacketLimit` | UdpHost | [] | bytes |
| `UdpHostPoll` | UdpHost | [tick] | → Actions |
| `UdpHostRead` | UdpHost | [request_id, peer_id, path] | → Actions |
| `UdpHostReceive` | UdpHost | [claimed_peer, attachment, packet_bytes] | → Actions |
| `UdpHostRemoteBreach` | UdpHost, Peer | [] | → Actions |
| `UdpHostRetireBefore` | UdpHost | [prefix, suffix] | → Actions |
| `UdpHostSetAttachment` | UdpHost | [peer_id, optional_attachment] | → Actions |
| `UdpHostSetService` | UdpHost | [peer_id, allowed] | → Actions |
| `UdpHostTransferState` | UdpHost | [peer_id] | 0 absent or [window, in_flight, queued] |
| `UdpHostUnbind` | UdpHost | [path] | → Actions |
| `UdpHostUsage` | UdpHost | [] | core_usage |
| `UdpLane` | UdpSocket! | [remote_address, max_packet] | → Lane |
| `V1Checksum` | — | [checksum_mode, bytes] | gum |
| `Verifier` | — | [total_bytes, proof] | → Verifier |
| `VerifierIngest` | Verifier | [fragment, pair_or_empty] | 0 |
| `VerifierRoot` | Verifier | [] | 32-byte hash |
| `VerifierState` | Verifier | [] | [next_index, complete] |
| `Verify` | PublicKey | [message, signature] | 0, or error |
| `WireDecode` | — | [bytes, wire_limits] | → Packet v2 |
| `WireEncode` | Packet | [] | wire bytes |
| `WireV1Decode` | — | [bytes, wire_limits, checksum_mode] | → Packet v1 |

## Records and enums

All byte and text fields below use bars. Omitted limits are `[]`, selecting
upstream defaults. Limits are positional records, not optional field maps.

- `noun_limits`: `[max_input_bits, max_output_bits, max_nodes, max_depth, max_atom_bits, max_backrefs]`.
- `wire_limits`: `[max_packet, max_path, max_fragment]`.
- `identity_limits`: `[max_blob_bytes, max_dat_bits, max_evidence_bits]`.
- `store_limits`: `[checkpoint_bytes, message_bytes, delivery_records, delivery_bytes]`.
- `core_config`: `[wire_limits, bloq, max_peers, max_bindings, max_binding_bytes, max_read_bytes, max_reassembly_bytes, max_unverified_bytes, max_reads, max_fragments, window, retry_after, read_timeout, max_attempts]`.
- `peer_fields`: `[id_u128, domain_text, life_u32, rift_u32, key_id_u64, service_allowed]`. Key IDs here belong to libmesa's configured key set;
  native handles themselves have no PLAN numeric ID table.
- `transfer_policy`: `[max_attempts, read_timeout, retry, congestion]`.
  Retry is `[0, after]` (fixed) or `[1, initial, maximum]` (exponential).
  Congestion is `[0, window]` (fixed) or `[1, initial, maximum]` (AIMD).
- `name_v2`: `[publisher_u128, domain_text, rift_u32, bloq_u8, fragment, path]`.
  V1 names omit domain: `[ship_u128, rift_u32, bloq_u8, fragment, path]`.
  Fragment is `[0, 0]` init, `[1, index]` auth, or `[2, index]` data.
- Packet body: `[0, name]` Peek; `[1, name, data, optional_next_hash]` Page;
  `[2, ack_name, payload_name, data]` Poke.
  Packet data is `[total_bytes, auth, fragment_bytes]`.
  Auth is `[tag, bytes]`: tags 0 none, 1 signature, 2 MAC, 3 pair.
  V1 signatures have fixed length 64; v2 validates according to its codec.
- Checksum mode: 0 HoonAtom, 1 VereBytes.
- `message_id`: `[sender_u128, sender_rift_u32, receiver_u128, receiver_rift_u32, flow_u64, sequence_u64]`.
- `outcome`: `[]` accepted, `[code_u16]` rejected.
- `read_kind`: 0 message, 1 receipt, 2 release, 3 released.
- `core_usage`: `[peers, namespace_slots, bindings, binding_bytes, binding_proof_bytes, reads, reassembly_bytes, pending_interests, received_fragments, unverified_bytes, unverified_fragments]`.
- `flow_usage`: `[outbox_messages, outbox_bytes, buffered_messages, buffered_message_bytes, pending_commit_bytes, retained_receipts, active_reads, stalled_reads, pending_commit, awaiting_decision]`.
- Relay leg: 0 A, 1 B. Time values are explicit monotonic libmesa ticks supplied
  by the host; the bridge does not create a clock or timer thread.

## Actions and durable commits

`ActionsNext` drains one item. Its data is a row beginning with a tag **bar**:

| Tag | Remaining fields |
| --- | --- |
| Send | peer_id, attachment, packet_bytes |
| ReadFinished | request_id, status (0 success / 1 error), bytes or error text |
| SetTimer | optional_tick |
| Dropped | diagnostic text |
| Commit | none; new_handles contains a CommitRequest |
| Deliver | message_id, bytes |
| Enqueued | message_id |
| CommittedDelivery, Settled | message_id, outcome |
| Stalled | flow_id, sequence, read_kind diagnostic text, failure diagnostic text |
| AnnouncementStalled | message_id, read_kind diagnostic text |
| EpochInvalidated | peer_id, new_rift |

Host methods return a data row of diagnostic notice bars and a separate Actions
handle. Process both, including the batch returned by `MesaHost` itself.
Diagnostic text is for display; do not parse Rust Debug strings as stable enums.

`CommitFields` returns a change row with a tag bar:
`[Enqueue, id]`, `[Deliver, id, bytes, outcome]`, `[Settle, id, outcome]`,
`[RetireReceived, id]`, or `[RetireSent, id]`.
Its exported token describes the proposed commit; exporting or constructing a
token does **not** commit anything. Only acknowledge the node with
`NodeCommitted` / `HostCommitted` after durable storage succeeds.

For the SQLite adapter, use `StoreCommit`, or `StorePrepare` followed by
`TransactionCommit`, then feed the resulting token to the node. While prepared,
all other operations on that Store are rejected. The Transaction retains the
connection even if its Store wormhole is closed or collected. Closing or
collecting an unfinished Transaction rolls it back. Reopening requires the
original empty scope checkpoint (revision 0); `StoreCheckpoint` then returns
the recovered current state. A new writer incarnation fences old writers.

`Host` owns its Node and Lane. `UdpHost` owns its Engine and socket. Their query
methods return copies or normal fields; no borrowed child handle can outlive
its native parent. Reconnection consumes the replacement Lane, including when
the native operation rejects it. Socket addresses are numeric IPv4/IPv6 with
ports; hostname resolution is not performed.
