# libmesa spec — 7: Transports (N-1 mappings)

Transport policy must describe the path between the Mesa endpoints. A
reliable local attachment does not establish a reliable path through relays.
`mesa-udp` now supplies a bounded nonblocking host for the
[signed-public profile](08-signed-public.md), tested between independent loopback
processes under fixed and adaptive [transfer policies](09-transfer-policies.md).
`mesa-transport` adds a [durable flow host, framed TCP attachment, and bounded
two-leg relay](11-flow-transports.md). Loopback socket tests exercise UDP and TCP;
a deterministic stream-to-datagram relay harness exercises downstream loss,
disconnects, and receiver restart. Policies remain explicitly selected by the
host. Local capability declarations do not establish complete-path reliability;
automatic complete-path policy selection remains unimplemented.

## Common contract

Each attachment declares max-SDU, reliability, ordering, datagram or stream
framing, lane representation, and duplex behavior. These properties describe
that attachment only. Endpoint policy retains retries and bounded interests
unless reliability is established for the complete path. Congestion and
backpressure must account for every lossy leg.

A relay may forward an unchanged pact only if the next attachment accepts
its size. Jumbo support requires a known path bound or a relay that explicitly
owns refragmentation and proof handling. A variable-width length field is not
evidence of path support.

## Mappings

| Mapping | Framing | Recovery and size policy |
|---|---|---|
| UDP | One pact per datagram | 1472-byte profile; bloq 13; retries and congestion control |
| Direct TCP between Mesa endpoints | Bounded length-prefixed pacts | TCP handles in-connection byte loss; retain bounded request completion, disconnect handling, and reconnect recovery; jumbo requires peer support |
| WebSocket | One pact per binary **message** | Same path requirements as TCP; assemble fragmented WebSocket frames under a size limit |
| WSS → relay → UDP | Pact on both attachments | Retain endpoint recovery and UDP-compatible sizes, or specify a relay that owns downstream recovery and refragmentation |

The last mapping is the phone bring-up profile. Suppressing endpoint retries
because its first leg is WSS would leave dropped UDP packets unrecoverable.
Turning off the endpoint congestion policy also requires a replacement for
the downstream UDP leg; TCP backpressure alone does not provide one.

Relays retain PIT state and expiration bounds. They are not stateless.
Details of reliable relay service, reconnect policy, jumbo negotiation,
STUN, hole punching, and transport-aware routing remain to be specified.
Lightning, Nostr rendezvous, Lick, and Mesa-over-Mesa are future mappings.
