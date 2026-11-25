# libmesa spec — 0: Scope and conventions

> **Scope outline.** Chapter 1 describes the implemented wire profile;
> chapter 5 defines the identity contract, with parsing and crypto primitives
> implemented but authority and lifecycle pending. Other chapters remain
> milestone drafts. Design rationale lives in `../doc/`.

## Outline

1. **Scope.** The Mesa protocol (packet, message, flow, namespace layers) and
   Fixed 128-bit identities under explicit domain authority, with the current
   Groundwire suite-C profile and PKI-domain lifecycle as
   specified by the composed gwbtc key-rotation/snob stack, restated
   transport- and host-independently. Native admission uses registered domains;
   life-1 fallback and provisional bootstrap are explicit opt-ins. Azimuth is
   a future alias source/domain adapter. The target name layout supersedes
   v1 width encoding (chapter 1). Mesa-only: no legacy ames, Fine, or `%ahoy`;
   proactive rekey interop requires an upstream Mesa announcement change.
2. **Conformance classes.** *Reader* (peek/page only) · *Peer* (+ flows) ·
   *Relay* (+ forwarding role) · *Verifier* (+ ≥1 PKI domain).
3. **Conventions.** RFC-2119 keywords. Mechanism vs policy: mechanisms are
   normative; policies have registries with mandatory-to-implement defaults
   (see `../doc/02-rina.md`). Vocabulary discipline: NDN terms in data-plane
   chapters, RINA terms in architecture chapters.
4. **Versions.** Experimental Mesa packet version 2 is separate from domain
   versions; version 1 is retained only behind an explicit compatibility API.
   The `%gw-btc` reference format commits kelvin **9** in its `dat`; other
   domains own their versioning. Unknown domain versions are unevaluable,
   never grounds to block the claimed identity.
5. **Golden vectors.** `../vectors/` is normative where present; conflicts
   between prose and vectors are spec bugs.
