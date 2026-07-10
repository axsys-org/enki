# ShrineGFX — No-API Foil Substrate Implementation Plan

**Lift the no-api backend machinery to the Foil level (typed records + laws + lawful write legs) over the landed op-83 host-call table, so that the chain `Foil/Rex IR → exact backend artifact bytes → root pointer command graph` runs with native code reduced to inert mechanics.**

> *Scope: the enki `ShrineGFX` branch only. Governs the five roadmap items (memory-arena/heap-pointer-complement, descriptor-heap, root-data-layout, hazard-fact/barrier, pipeline-artifact cache) plus their Foil record layer. Does NOT cover: store hardening (root generations / watch-poll — separate lane), the Rex policy layer (`@active` backend selection, capabilities — lands when sys-meta projection ports), CAMetalLayer present/surface arm, MoltenVK target, or the PSA microcosm content itself (PSA is the first consumer, not part of this plan; see Exit Criterion).*
>
> *Layering rule this plan enforces (agreed 2026-06-11): **Rex notation** holds policy and authored declarations; **Foil typed records + PLAN laws** hold derived carriers, layouts, hazards, and evidence; **native op-83 jets** hold irreducible mechanics. A jet that interprets payload fields, picks a fallback, or owns identity is a layering bug.*

### Reference Index

| Reference | Governs |
|-----------|---------|
| Compute-Backend-Substrate-Spec.md (CB) | Canonical flow, row families (CB4), memory model (CB8), command/hazard model (CB9), artifact/reflection complements (CB10), refusals (CB12) |
| PLAN-Lawful-Rex-Substrate-Spec.md (PLR) | Identity (pin + occurrence + evidence), byte-index discipline (PLR2), view records (PLR10), host floor (PLR17) |
| Trefoil-Admitted-Boundary-Substrate-Spec.md (TAB) | ByteCarrierAdmit shape the carrier records converge toward |
| Adjoint-Classical-Logic-Substrate.md (ACL) | Mode discipline: request/artifact = Producer, carriers/graphs/root bytes = Linear residuals, witness/refusal/readback = Consumer; backend emission as the ↓L_C shift (ACL7.2); Gap 2's generation rule and Gap 5's hazard transfer are the linear no-contraction discipline operationalized |
| Personal-Namespace-Web-Spec.md (PNW) | The eventual consumer: the Web host-IO scheduler (PNW10, PNW.15–16a) batches admitted requests via `io-scheduler-batch` rows carrying `effective_bytemap` + `hazard_cone` — Gap 5's hazard-fact rows feed both the encoder barriers and that scheduler; the present arm hangs off the Web root, not a window manager |
| `NoApiMetalBackend.m` (shrine-collapse, EnkiBridge) | Metal port source: admitted heap arena, descriptor-heap evidence, root-data shape checks, pin-keyed pipeline cache, 9xx refusal kinds |
| `github.com/UnNabbo/no_api` (clone at /tmp/no_api) | Vulkan reference: interval-table pointer resolution (the implicit pointer complement), descriptor-buffer bytes-at-index×stride, Stage×Stage×Hazard barrier triple, split barriers, timeline semaphores |
| `pkg/plan/hostcall.{h,c}`, `pkg/host/src/gpu_metal.m`, `tests/unit/test_hostcall_gpu.c` | The landed Gap 0 substrate this plan extends |

---

### Gap 0 — op-83 binding table + gpu.* Metal slice

> *Size: LARGE | Phase: A (foundation)*
> **Status: ✅ DONE (2026-06-11)**

The admitted host-call boundary and the first vertical slice: a pinned, handle-free materialization request drives session → library → pipeline → target → buffer → draw → readback, every step a witness/refusal row.

**What was implemented**:
- `pkg/plan/include/plan/hostcall.h` + `src/hostcall.c` — op-83 registry sharing the `pl_opdesc` shape; duplicate-binding refusal; `pl_op_desc` unified descriptor view; witness/refusal row builders (`(0 "witness" binding payload…)` / `(0 "refusal" binding kind detail)`)
- `eval.c` — opset-83 gate on `pl_thread.hostcall_f`, dynamic lookup via `pl_op_lookup_all`
- `pkg/host/src/gpu_metal.m` — 8 session-owned Metal thunks (941–949 refusal kinds, type-selected pipeline functions, bar-convention payloads)
- Makefile `pkg/host` layer (Darwin-only, ObjC+ARC, layering-checked); wisp registration + `hostcall_f` follows the RPLAN gate
- `tests/unit/test_hostcall_gpu.c` — 4 tests incl. pixel-asserted offscreen render from a store-reloaded pin; `foil/shrine/gfx-smoke.plan` wisp-level smoke (`(#pin "S")`)

**Pass criteria**: (1) duplicate registration refused — PASS (test); (2) ungated dispatch raises — PASS (test); (3) pinned request → render → center pixel `A=255,R=255,G≈128,B=0`, corner = clear, OOB readback = refusal 948 — PASS on Metal hardware; (4) full suite + check-layering — PASS (exit 0).

**Snippet**: Known debts this plan pays down: `gpu.render-quads` knows the 32-byte QuadRow stride (semantic shape in native → Gap 3); buffers are bare object handles, not pointer complements (→ Gap 2); `waitUntilCompleted` is the sync model (→ Gap 5); library/pipeline intern is per-session, not pin-keyed (→ Gap 6).

---

### Gap 1 — Foil carrier records for the no-api substrate

> *Specs: CB4 (row families), CB8 (memory model), PLR2 (byte index), PLR10 (view records)*
> *Size: MEDIUM | Phase: B (everything below consumes these shapes)*
> **Status: ✅ DONE (2026-06-11)**

One wisp module declaring the carrier record shapes, so jets and tests stop improvising row layouts. Landed idiom: a carrier record is an **under-applied pinned law** — constructor = plain application, record kind = law name, fields = app args (`Ix i rec` from wisp, `pl_app_args` from C), and a trailing never-supplied arg keeps the closure in WHNF as data. Chosen over `[..]`/`{..}` row literals because those parse to reaver-prelude macros that `foil/shrine` modules cannot include (`@module` includes are same-directory only); the law idiom is dependency-free and gives kind-checking for free.

**What was implemented**:
- `foil/shrine/no-api-carriers.plan` (~170 lines): 13 record constructors (`memory-arena`, `heap-pointer`, `descriptor-heap`, `descriptor-window`, `root-segment`, `root-layout`, `hazard-fact`, `command-step`, `command-graph`, `artifact`, `pipeline-request`, `gfx-request`, `pair` with nil = 0) + B-op wrappers + bar algebra (`bar`, `bar-len`, `bar-data`, `bar-weld` via `Bytes/Trunc/Lsh/Add` nat arithmetic — `Weld` is row-arg weld, NOT byte concat) + the Gap 3 `instantiate-root` law
- Law-body convention: nat 1 (first-field ref) in both wisp and C constructions, so the shapes pin-hash identically
- Wisp smoke `main`: constructs every record, composes a `gfx-request` v2, Traces field access + the two-segment weld fixture
- C side: `gm_record(v, kind, nfields)` resolver in `gpu_metal.m`; `record()` builder in the test

**Checklist**:
- [x] Record constructors for the row kinds (accessor = generic `Ix`; per-field laws deferred until a consumer needs them)
- [x] Gap 0 request re-expressed: slice test composes carrier records for arena/pointer/graph (the pinned request row itself stays flat — handles never appear in it, composition happens at dispatch)
- [x] Wisp smoke constructs each record and Traces it
- [x] Build + verify: suite green; `wisp foil/shrine no-api-carriers main` prints the composed request canonically

**Pass criteria**: (1) records construct, pin, and field-access from wisp — PASS (smoke output: capacity 65536 at `Ix 1`, generation at `Ix 4`, canonical `(gfx-request (artifact …) (root-layout …) …)` print); (2) slice test passes through composed carriers — PASS; (3) build + suite — PASS.

**Snippet**: Fields may arrive as thunks inside law-built records — jets normalize record args with `pl_nf(t, ARG(i))` before `gm_record`. `heap-pointer` field 0 is the allocation slot in the session's single arena; a sub-window is the same record with narrowed offset/length.

---

### Gap 2 — memory-arena + heap-pointer-complement jets

> *Specs: CB8 (one logical heap ABI; pointer = materialization complement), PLR10 (heap-range-view)*
> *Size: LARGE | Phase: B*
> **Status: ✅ DONE (2026-06-11)**

Make native allocate from an admitted arena carrier and return **pointer witnesses** `(arena, offset, length, generation)` instead of bare object handles. This is where complements become real heap handles. no_api's interval tables prove the metadata is unavoidable on any backend — this gap makes it declared data instead of hidden native state.

**What exists**: `gm_buffer` in `gpu_metal.m` — device-allocated `MTLBuffer` per call, object-handle witness, no arena, no generation. `NoApiMetalBackend.m:direct_metal_heap_prepare_admitted_arena` is the port source (MTLHeap from admitted byte size, 4 KiB-aligned, shared storage). no_api's `GPU_Allocator` shows the dual host/device address mapping and pointer-arithmetic-via-`offset = ptr − base`.

**What's missing**:
- `gpu.arena` jet: MTLHeap from a Gap 1 arena carrier; arena witness with capacity + generation
- Rework `gpu.buffer` → `gpu.alloc`: sub-allocate from the arena (`newBufferWithLength` on the heap per the reference), write payload bytes, return pointer-complement witness; keep a per-session pointer table (index → MTLBuffer + generation) as the *bridge*, mirroring no_api's interval table but indexed by complement, not raw address
- `gpu.free`: release + **generation bump**; stale-generation refusal (new kind 950) on any use of an outdated complement. Per ACL6.2, a pointer complement is a **linear residual**: free consumes it, and reuse without an admitted duplication/projection law is contraction the mode discipline forbids — the refusal is the structural rule, not defensive coding
- Derived sub-window support: a pointer complement narrowed by (offset, length) is valid without a new allocation (no_api's mid-allocation pointers, made explicit)
- `gpu.device-address` (optional, Metal 3 `gpuAddress`): device-pointer witness for future shader-side pointer chasing

**Scope expectations**: one arena per session initially (the reference's shape); multi-arena and READBACK/DESCRIPTORS scopes (no_api's `Memory_Kind`) can be fields on the arena carrier from day one but only DEFAULT-scope needs to materialize now. Suballocator is bump-only — `gpu.free` may only release whole buffers; real reuse is a later allocator concern, not correctness.

**Work**: ~180 lines in `gpu_metal.m`; ~40 lines carriers/tests.

**What was implemented** (`pkg/host/src/gpu_metal.m`):
- `gpu.arena` (session, memory-arena record) → MTLHeap, 4 KiB-aligned shared storage; refusals: 951 bad carrier, 948 capacity range, 949 double-arena, 946 heap failure
- `gpu.buffer` deleted → `gpu.alloc` (session, bytes-bar): `newBufferWithLength` on the heap, payload memcpy'd, pointer-complement witness `(alloc, 0, length, generation)`; freed slots are reused at their bumped generation
- `gpu.free` (session, heap-pointer record): releases the buffer, bumps the slot generation — the linear consume; refusal kinds 950 (stale/freed), 942 (unknown slot), 948 (bad window)
- `gm_pointer` resolver validates every consumer's complement: liveness + generation + `offset + length ≤ allocation` (sub-windows accepted without new allocation)

**Checklist**:
- [x] `gpu.arena` from carrier; capacity refusal when heap creation fails
- [x] `gpu.alloc` returns (alloc, offset, length, generation) witness; payload written via heap buffer
- [x] `gpu.free` bumps generation; stale use refuses with 950 + structured detail
- [x] Sub-window complement accepted by consumers (`gm_pointer` window math; OOB window refuses 948 — exercised by test)
- [x] Build + verify: slice green with arena-allocated root; stale-generation + slot-reuse tests added
- [x] `gpu.device-address` — landed with the exit-criterion fixture (motor-sandwich root embeds in/out device addresses; arena resident via `useHeap`); `gpu.read` added as the buffer Consumer leg

**Pass criteria**: (1) render slice passes with the root payload in an arena allocation — PASS (pixel assertions unchanged); (2) use-after-free refuses 950 as data — PASS (test: freed complement → graph refusal 950, then realloc reuses the slot at generation+2); (3) window bounds enforced — PASS (length×2 window → 948); (4) suite green — PASS.

---

### Gap 3 — root-data-layout + generic command graph

> *Specs: CB8 (root payload as only per-dispatch entry), CB9 (generic payload kinds; "no ABI named after a demo"), CB16 (no workspace-specific payload ABI)*
> *Size: LARGE | Phase: B (the ABI-breaking one — do with Gap 2, before more callers exist)*
> **Status: ✅ DONE (2026-06-11)**

Kill the semantic shape in native. `gpu.render-quads` knows "32-byte QuadRow" — that knowledge moves into a Foil root-data-layout row plus a **PLAN law that instantiates the root bytes** from pointer/window carriers; native gets one admitted root carrier and a command graph of generic payload kinds it never interprets.

**What exists**: `gm_render_quads` (stride check `count * 32 > length`, fixed clear color, fixed vertex count 6). no_api's root model (two device pointers as push constants; shaders chase) vs. our buffer(0) root — both are "one entry point"; the layout row should declare the struct and leave push-constant-vs-buffer as a per-pipeline lowering field. `NoApiMetalBackend.m:direct_metal_root_data_from_product` + `…_shape_ok` show the shape-coherence checks (counts must agree across layout/payload/graph) that become law checks.

**What's missing**:
- Root instantiation as a PLAN law in `foil/shrine/`: walk the root-data-layout row, assemble root bytes from pointer/window carriers (pure byte concat/offset arithmetic — **the first executable materialization law**)
- `gpu.command-graph` jet replacing `render-quads`: consumes (session, command-graph carrier, root pointer-complement, target, pipeline); iterates command-steps by payload kind — `draw {vertex-count, instance-count}`, `dispatch {x,y,z}`, `clear {color}` — binds root at buffer(0), never reads payload fields
- Shape-coherence refusals ported from the reference (932-style): step counts vs. graph row, instance count × stride vs. root window, missing layout → refuse before encode
- Clear color moves from native constant into the graph's `clear` step (it was `#17110d` semantics hiding in C)

**Scope expectations**: `draw-indexed`/`indirect`/`blit`/`readback-step` payload kinds are enum values reserved in the carrier now, materialized later (no_api shows their Vulkan lowering; Metal equivalents are mechanical). Push-constant root lowering is a declared-but-refused mode until a pipeline needs it (Metal: `setVertexBytes` ≤ 4 KB).

**Work**: ~120 lines wisp law + carriers; ~150 lines `gpu_metal.m` (new jet, delete `render-quads`); test rework.

**What was implemented**:
- `instantiate-root` law in `foil/shrine/no-api-carriers.plan`: walks a pair-list of root-segments and welds their bars in declared order via `bar-weld` (nat arithmetic — `Bytes/Trunc/Lsh/Add`; PLAN's `Weld` primop is row-arg weld, not byte concat, so byte assembly is arithmetic). **The first executable materialization law.**
- `gpu.command-graph` jet (graph-record, session, pipeline, target, root heap-pointer, dheap-or-0): two-phase — validate every step/hazard record then encode; payload kinds `draw {vertex-count, instance-count}`, `clear {bgra8}` (first step only → load action), `dispatch` reserved-refused; root bound blindly at buffer(0) via the pointer complement; `gpu.render-quads` deleted
- Shape refusals: 951 carrier-shape, 952 graph-shape (non-pair steps, non-step records, mid-graph clear, count mismatch, zero-extent draw, non-hazard-fact hazard rows)
- Clear color moved from native constant into the graph's `clear` step

**Checklist**:
- [x] Instantiation law produces byte-identical root vs. hand-packed (law fixture: two-segment `"roo"+"t!"` weld Traces as `"root!"` in the wisp smoke)
- [x] `gpu.command-graph` executes draw + clear generically; `gpu.render-quads` deleted
- [x] Shape-mismatch refusals (952 family; stride overrun is the pointer window check from Gap 2 — native knows no stride)
- [x] Build + verify: slice renders identically through the generic graph

**Pass criteria**: (1) pixel assertions unchanged — PASS; (2) `grep -i quad pkg/host/` finds nothing — PASS (root payload is opaque bytes; row meaning lives in the artifact + layout); (3) malformed graph refuses with structured kind — PASS (952 tests); (4) suite green — PASS.

**Snippet**: Float payloads can't be authored in wisp, so the C slice allocates the payload bar directly; the law fixture proves weld-order byte identity on string bars. When Rex-side authoring lands, `instantiate-root` consumes `root-segment` rows whose bars come from admitted sources.

---

### Gap 4 — descriptor-heap

> *Specs: CB4 (descriptor-heap rows), CB8 (32-bit texture heap indices), RBG8.6 (Aaltonen surface)*
> *Size: MEDIUM | Phase: C (needs Gaps 1–2; first consumer is textured draw)*
> **Status: ✅ DONE (2026-06-11)**

Descriptor heap/window rows + native materialization. no_api proves descriptors are just bytes at `index × stride` in mapped memory (`vkGetDescriptorEXT` into a DESCRIPTORS-kind allocation); Metal's equivalent is `MTLResourceID`s written into an argument buffer at `index × 8`, with `useHeap`/`useResource` residency.

**What exists**: nothing native; `descriptor-window` carrier lands in Gap 1. `NoApiMetalBackend.m:direct_metal_descriptor_heap_from_product` shows the evidence shape (count + mixed witness over window fields; zero-field windows refuse).

**What's missing**:
- `gpu.descriptor-heap` jet: argument buffer sized from the heap carrier; per-window `gpu.descriptor-write` (texture handle → ResourceID at index) with generation echo
- Heap binding in `gpu.command-graph` (fragment buffer slot 1, after the root); residency calls
- Sampled-texture target usage (`MTLTextureUsageShaderRead`) and a textured-quad MSL fixture indexing the heap by u32 from the root payload
- Stride-class field honored (combined-sampler vs storage distinction is a tag now; Metal collapses both to ResourceID — record it anyway, MoltenVK will need it, per no_api's two descriptor sizes)

**What was implemented**:
- `descriptor-heap` carrier (count, stride-class) in `no-api-carriers.plan`
- `gpu.descriptor-heap` jet: argument buffer of `count × 8` zeroed bytes from the carrier; witness `(dheap, count)`
- `gpu.descriptor-write` (session, dheap, descriptor-window record, texture): `MTLResourceID` memcpy'd at `index × 8`; window must name the same heap (951), match the heap generation (950), and index inside count (948); written textures tracked for residency
- `gpu.texture-2d` (session, w, h, bgra8-bar): sampled texture upload via `replaceRegion`; payload must be exactly `w*h*4` bytes (948)
- `gpu.command-graph` gained the dheap-or-0 arg: heap bound at fragment buffer(1), written textures made resident via `useResource` per pass
- Sampler: **constexpr in the artifact** — zero API sampler objects, which is one better than the planned per-session static (and avoids no_api's leak entirely)

**Checklist**:
- [x] Heap materializes from carrier; writes land ResourceIDs at index×stride
- [x] Textured draw samples via heap index (pixel-asserted: 2×2 quadrant texture, four readback probes exact-match red/green/blue/white)
- [x] Stale-generation window write refuses 950; bad index refuses 948; cross-heap window refuses 951
- [x] Build + verify: `gfx_textured_draw_samples_descriptor_heap` green

**Pass criteria**: (1) shader reads its texture purely via the heap — the fragment function names `heap[0].tex`, no per-draw binding call names a texture — PASS; (2) descriptor witness carries count; window writes echo (index, generation) — PASS (window hash deferred to admission); (3) suite green — PASS.

---

### Gap 5 — hazard-facts + barriers

> *Specs: CB9 (hazards are declared facts; barriers are stage facts, not resource-state fiction), RBG8.6 (hazard cone ≡ TrefoilMeasure at byte windows)*
> *Size: MEDIUM | Phase: C*
> **Status: ✅ DONE (2026-06-11 — schema/witness/sync landed in Slice 1; the deferred negative fixture and split-barrier pairs closed same day by Slice 2 Gap 8: wait-value dependencies + deterministic adversarial-scheduling RAW fixture)**

Command-graph execution consumes declared hazard facts instead of conservative waits as correctness. Adopt no_api's minimal schema verbatim — `(before-stage-mask, after-stage-mask, access-class)` lowered to one global memory barrier — plus split-barrier (event) pairs for cross-pass overlap.

**What exists**: `gm_render_quads` commits + `waitUntilCompleted` per call — full serialization as the sync model. no_api's `gpu_barrier` (Stage×Stage×Hazard → `VkMemoryBarrier2`) and `gpu_signal_after`/`gpu_wait_before` (VkEvent split barriers); `Hazard` access classes {DRAW_ARGUMENTS, COLOR_BUFFER, DEPTH_STENCIL, DESCRIPTORS}. Note no_api's Vulkan image-layout tracking does NOT port — Metal has no layouts; that whole `transition_image_layout` axis disappears on our backend.

**What's missing**:
- Hazard-fact rows attached to command-graph edges (Gap 1 carrier; populated by the graph author — eventually the bytemap audit, for now the request). The same fact rows are what PNW10's `io-scheduler-batch` carries as `hazard_cone` when the Web host-IO scheduler becomes the caller — author them once, consume twice
- Native lowering inside `gpu.command-graph`: between steps, declared facts → `MTLRenderCommandEncoder` memory barriers / pass splits / `MTLFence` for split pairs
- `waitUntilCompleted` demoted to the readback boundary only: `gpu.submit` returns a submission witness (command buffer + `MTLSharedEvent` value, no_api's timeline-semaphore shape); `gpu.readback` waits the event, not the world
- Hazard evidence in the execution witness (the reference's `direct_metal_command_hazard_witness` mix)

**Scope expectations**: with one queue and offscreen targets, barriers are nearly invisible behaviorally — the *verifiable* part now is the schema, the witness, and that an undeclared-hazard graph with a RAW dependency can be constructed to produce a wrong pixel when barriers are skipped (negative fixture) and a right one when declared. Multi-queue and cross-encoder fences are deferred to the present arm.

**What was implemented**:
- Hazard-fact rows on the command-graph carrier (stage masks {1 vertex, 2 fragment}, access classes {1 draw-arguments, 2 color-buffer, 3 descriptors}); non-fact rows in the hazard list refuse 952
- **Lowering = pass split, not `memoryBarrier`**: within one Metal render pass, draw order to the attachment is hardware-guaranteed and intra-pass memory barriers are restricted on Apple-family GPUs — the TBDR-native barrier between draw steps is ending the encoder and reopening with `LoadActionLoad` (`gm_open_pass` rebinds pipeline/root/heap/residency). Absent facts = single pass, zero splits — never a conservative wait
- Submit/event model: the graph commit encodes `MTLSharedEvent` signal at `++event_value`; the witness carries `(executed, splits, event-value)`; `gpu.readback` does `waitUntilSignaledValue:timeoutMS:5000` (timeout → 947) — **`waitUntilCompleted` appears nowhere**
- Fixtures: 2-draw graph without facts → splits 0; with one fact → splits 1, event value monotonic, pixels still correct through the split pass

**What's deferred and why**:
- Negative RAW pixel fixture (undeclared hazard → wrong pixel) — **Why**: not constructible single-encoder: Metal guarantees primitive order to the attachment within a pass, so an undeclared intra-pass RAW cannot render stale deterministically. The observable divergence needs a cross-encoder dependency (second pass sampling the first's target) — **When**: the present arm / first multi-pass graph, where pass-sequencing makes the declared-vs-undeclared difference physical. (Compute graphs narrowed this gap: dispatches run `MTLDispatchTypeConcurrent`, so an undeclared compute RAW is now a *real* race, just not a deterministically assertable one)
  — **CLOSED (2026-07-09)** by the first multi-pass render chain (`gfx_cross_pass_raw_pixel_divergence`): pass A renders into target T, pass B samples T through the descriptor heap into target U (targets are already `RenderTarget|ShaderRead` and intern as ordinary textures, so a target handle written into a descriptor window is the whole bridge — zero new native mechanics). Determinism by adversarial scheduling per CB9/Gap 8: undeclared → B runs to completion before A is even submitted and samples the stale seed color, every time; declared → B waits A's signal value and samples the produced pixel.
- Split-barrier (event) pairs for cross-pass overlap and `MTLFence` — **When**: same trigger

**Post-closure addendum (2026-06-11, exit-criterion work)**: compute graphs landed the *real* barrier lowering — declared facts between dispatches emit `memoryBarrierWithScope:Buffers` inside a concurrent-dispatch encoder (witnessed in the lowered count). The pass-split lowering remains the render-side story only.

**Checklist**:
- [x] Hazard rows lower between draw steps (as pass splits); absent facts = no split
- [x] Shared-event witness on the graph; `gpu.readback` waits the event value only (`gpu.submit` folded into `gpu.command-graph` — one queue, commit is submission)
- [x] Negative RAW fixture — closed 2026-07-09 (`gfx_cross_pass_raw_pixel_divergence`: cross-pass render-to-texture-then-sample; see above)
- [x] Build + verify: slice green under the event model; `grep waitUntilCompleted pkg/` → zero call sites

**Pass criteria**: (1) `waitUntilCompleted` appears nowhere; the only wait is the readback event — PASS (grep); (2) declared-vs-undeclared fixtures: splits 1 vs 0 with identical correct pixels — PASS (witness evidence; pixel divergence closed 2026-07-09 by the cross-pass fixture); (3) suite green — PASS.

---

### Gap 6 — pipeline-artifact cache

> *Specs: CB10 (pipeline realizes artifact — second relation, separately witnessed), CB16 (no per-frame pipeline construction)*
> *Size: SMALL | Phase: C*
> **Status: ✅ DONE (2026-06-11)**

Materialize native pipelines keyed by artifact pin and reuse them; cache evidence in the witness. no_api has no cache at all (fresh pipeline + fresh layout per create) — the reference `NoApiMetalBackend.m` cache (`direct_metal_cached_render_pipeline_for_product`, keyed `(pipeline_pin, artifact_pin, layout_pin)`, hit-count evidence) is the port source.

**What exists**: `gm_library`/`gm_pipeline_render` intern per session by handle, but a second identical request recompiles. Artifact bytes already arrive as pinned bars, so the key exists.

**What was implemented**:
- Process-level static caches (matching the reference) keyed by **sha-256 of the artifact payload bytes** — the same bytes the artifact pin commits to, so the key is the pin's content address without needing the pin object at the jet boundary
- `gpu.library` and `gpu.pipeline-render` both take the artifact bar; pipeline creation resolves its library from the library cache by the same key (the CB10 "pipeline realizes artifact" relation, separately witnessed)
- Both witnesses carry `(handle, hits)`; hits = prior cache serves (0 = cold build)

**Checklist**:
- [x] Pin-keyed (content-address-keyed) library + pipeline caches; hit increments evidence
- [x] Layout ref in the key — closed by Slice 2 Gap 9: the key is sha(sha(artifact) ‖ stride ‖ mode), and stride/mode are exactly the root-layout fields that reach the pipeline lowering (count/segments shape only the root bytes, never the descriptor)
- [x] Build + verify: double-create shows hit=1 on second call (asserted in the slice test)

**Pass criteria**: (1) identical request compiles once — PASS; (2) witness carries hit evidence — PASS (slice asserts hits 0 then 1); (3) suite green — PASS.

---

## Build Order

```
Gap 0 (op-83 table + slice) ✅
  └─ Gap 1 (Foil carrier records) ✅          — every later gap consumes these shapes
       ├─ Gap 2 (arena + pointer complement) ✅ — ABI break #1: alloc returns complements
       ├─ Gap 3 (root layout + command graph) ✅ — ABI break #2: render-quads deleted
       │    ├─ Gap 4 (descriptor heap) ✅      — binds through the graph
       │    ├─ Gap 5 (hazard facts) ✅         — lowers inside the graph (negative fixture closed by Slice 2 Gap 8)
       │    └─ Gap 6 (pipeline cache) ✅       — keyed by artifact content address
```

**Executed sequence (2026-06-11)**: 1 → 2+3+6 in one stroke (the rewritten `gpu_metal.m` carried all three ABI changes while the slice test was the only caller) → 4 → 5. State at close: 6 GPU tests + full suite (84 tests, 9 binaries) + `check-layering` green; both wisp smokes pass; `gpu_metal.m` ~1,070 lines.

## What Each Phase Unlocks

| After | New capability | Status |
|---|---|---|
| Gap 0 | PLAN values invoke admitted GPU mechanics; pixels prove it | ✅ Done |
| Gap 1 | Carriers are declared data; jets and tests share one row vocabulary | ✅ Done |
| Gaps 2+3 | The CB canonical flow is real: pinned artifact + law-built root + generic graph, zero payload semantics in native | ✅ Done |
| Gap 4 | Textured/sampled work; the 32-bit-index heap of RBG8.6 | ✅ Done |
| Gap 5 | Frame-overlap-capable execution; sync is declared facts + event witnesses | ✅ Done (completed by Slice 2 Gap 8) |
| Gap 6 | Per-frame reuse; pipeline construction off the hot path | ✅ Done |

## Exit Criterion (the proof this plan is done)

> **Status: ✅ LANDED (2026-06-11)** — `gfx_motor_sandwich_dispatches_through_graph`.

The **PSA motor-sandwich kernel as first consumer**: `meta-psa-substrate`'s sandwich op emitted as an MSL artifact (ported from the shrine corpus emitter), pinned, run as a `dispatch` step through the generic command graph against an arena-allocated root of motor/blade rows, readback witness matching the CPU table-walker — graphics machinery exercised end-to-end by non-graphics work, which is CB2's "compute as universal microcosm" claim made testable. It is also ACL4.2 made literal: the sandwich is the canonical adjoint composition, so this fixture is the producer-linear-consumer triangle (`↑P_L` admit → L-mode table walk on device → `↓L_C` readback witness) executing on hardware. When it passes, the no-api Foil substrate is real and the Rex policy layer (`@active` targets, capabilities, sys-meta roles) becomes the next plan.

**What was implemented** (re-read of NoGraphicsApi.md + the Aaltonen transcript steered the design):
- `gpu.pipeline-compute`: the library's **unique kernel function** (selected by type) — one entry point, the CUDA/no-api discipline; cached by artifact content address like render, separate cache
- **Compute path in `gpu.command-graph`** (same jet, pipeline kind selects): `MTLDispatchTypeConcurrent` encoder — absent facts mean dispatches genuinely overlap, never an implicit serialization; a declared compute→compute hazard-fact lowers to a **real `memoryBarrierWithScope:Buffers`** (compute encoders support it; only render passes were restricted to splits). Dispatch step = `{groups-x groups-y groups-z threads-x}`; threadgroup width validated against the pipeline
- `gpu.device-address`: 64-bit GPU virtual address of an admitted complement window — "all pointers in GPU data structures must use GPU addresses." Root payloads embed these; the whole arena goes resident (`useHeap`, both encoder kinds); native never chases the addresses, shaders do
- `gpu.read`: buffer readback through the complement (Consumer leg for non-pixel evidence); waits the session event like `gpu.readback`
- `gm_library` compiles with `MTLMathModeSafe` — fast-math reassociation would break CPU=GPU table-walker matches
- The fixture: rotor+translator motor sandwich over 8 point rows; root struct = `{q, t, in_addr, out_addr, count}` at buffer(0), in/out rows in sibling arena allocations referenced **only by device address**; two dispatches + one declared fact → witness `(executed 2, barriers 1, event)`; `gpu.read` floats match the CPU walker at 1e-5; draw-in-compute-graph refuses 952

**Honest residue**: the MSL artifact is hand-authored in the test, not yet emitted from `meta-psa-substrate`'s sandwich table — the kernel *shape* (one entry point, root pointer, address-chased rows) is exactly what the Rex-side emitter will produce, and porting that emitter is the first task of the Rex policy layer plan. The full PGA antiproduct-table walker (vs. the quaternion-form motor here) ships with that emitter.

---

## Slice 2 — The Lengyel Pipeline (resolves every Slice-1 deferral)

> *Specs: CB6 (foil-msl emitter class), CB9 (hazards gate scheduling; command-dependency),
> CB10 (pipeline second relation keyed by root-data policy), ACL5/ACL9 (one mode-uniform
> table-walker body; per-subalgebra closed forms are the documented drift), ACL7.1 (PSA ops
> are L-mode arrows), Lengyel "Relativistic Quaternions" 2024 (eq. 11 motion operator Q,
> eq. 16 dual quaternion D, eq. 17 bulk norm as the subluminal admissibility predicate)*
> **Status: ✅ DONE (2026-06-11) — all four gaps landed same day; 11 GPU tests, 9 suites,
> layering + format green**

All four Slice-1 deferrals share one missing trigger: a **multi-stage pipeline with a
cross-stage dependency**. The Lengyel paper supplies it: (A) antiproduct-sandwich
motion-operator rows through the mode-uniform 32-blade table walker → (B) bulk-norm each
result; eq. 17's real-norm test is a physical refusal predicate (real ⟺ subluminal ⟺
admitted) writing indirect dispatch args for the surviving rows → (C) dispatch-indirect
over only the admitted rows. A → B → C is the RAW chain every deferral was waiting for.

### Gap 7 — foil-msl emitter law (kills the hand-authored-artifact residue)

CB6's steady-state emitter class in miniature: a wisp law computes the R(3,1,1)
antiproduct sign/index table (metric (−1,1,1,1,0) over 32 basis blades; nat arithmetic on
basis bitsets) and a second law **welds the MSL kernel text from fragment bars + the
emitted table constant** via `bar-weld` — Foil values writing exact backend bytes through
the lawful byte leg that already exists. One table-walker kernel body for every
active-component subset (Q's 8 slots, D's 12, full motor 16 — ACL5: never a per-subalgebra
closed form). The C test stops owning the artifact; it pins the law-emitted bar.
*Pass criteria*: (1) law-emitted table matches a CPU-computed reference table entry-for-entry;
(2) the emitted artifact compiles and the sandwich matches the CPU walker (replacing the
quaternion-form fixture); (3) suite green.

### Gap 8 — submission dependencies + split-barrier pairs → deterministic negative fixture

`command-graph` gains a 4th field `wait-value` (0 = none) lowered to `encodeWaitForEvent:`
before encoding; the signal value is already in every witness — Aaltonen's
`gpuSignalAfter`/`gpuWaitBefore` with the session event as the timeline counter.
Independent graphs submitted between a signal and its wait overlap freely (the
cascading-overlap shape). **The negative RAW fixture becomes deterministic by adversarial
scheduling, not racing**: per CB9, hazards gate *scheduling* — absent a declared
inter-graph fact, submission order is unconstrained, so the fixture exercises that freedom
by completing the consumer graph before submitting the producer (stale bytes, every time,
lawfully). With the fact declared → wait-value ordering → correct bytes. The same facts
are what PNW10's `io-scheduler-batch` carries as `hazard_cone` — authored once, consumed
twice. ABI break (record arity 3→4) while the test is the only caller, same rationale as
Gaps 2+3.
*Pass criteria*: (1) consumer-before-producer without a declared fact reads stale bytes —
deterministically; (2) declared fact → wait lowering → correct bytes; (3) an independent
graph between signal and wait completes without waiting (witness event values prove
overlap); (4) suite green.

### Gap 9 — pipeline-request consumed: layout-keyed cache + inline-root lowering

The Gap 1 `pipeline-request (artifact, layout)` carrier becomes the pipeline jets' real
input. `root-layout` declares the lowering: buffer-bound root (today) or **inline root**
via `setBytes`/`setVertexBytes` (≤ 4 KB — the Metal push-constant analogue; the 64-byte
motor root is the first user). Cache key = sha(artifact ‖ layout fields) — CB10's second
relation ("pipeline realizes artifact *under this root-data policy*") witnessed literally.
The pointer complement remains the authority either way; only the binding lowering differs.
*Pass criteria*: (1) same artifact under two layouts → two pipelines, cache miss across,
hit within; (2) inline-root dispatch produces bytes identical to buffer-root; (3) suite green.

### Gap 10 — dispatch-indirect, GPU-written by the admissibility stage

Step kind 4 `dispatch-indirect {alloc, offset, generation, threads-x}`: validated through
`gm_pointer` (12-byte window = MTLDispatchThreadgroupsIndirectArguments), lowered to
`dispatchThreadgroupsWithIndirectBuffer:`. The args bytes are **written by stage B**: the
bulk-norm pass counts rows whose eq.-17 norm is real and emits the threadgroup counts —
CB3 refusal predicates executing on-device, the GPU feeding itself. Native interprets
nothing; it points Metal at an admitted window. `draw-indexed` stays reserved (one extra
pointer-complement arg when a mesh consumer appears); `blit`/`readback-step` wait for the
present arm.
*Pass criteria*: (1) B admits k of n rows; C's indirect dispatch processes exactly k
(readback proves untouched refused rows); (2) stale-generation args window refuses 950;
(3) suite green.

**Build order**: 8 (dependencies — the chain's spine) → 9 (inline root, independent) →
10 (needs 8's multi-graph chain) → 7 (pure wisp, parallelizable with all of the above; its
emitted artifact replaces the fixtures' MSL as the last step). Exit: the A→B→C Lengyel
chain runs end-to-end with a law-emitted artifact, declared dependencies, an inline root,
and an indirect middle — all four Slice-1 deferrals closed by one consumer.

**Execution record (2026-06-11)** — all pass criteria PASS:

- **Gap 8** (`gfx_dependency_facts_gate_scheduling`): command-graph carrier arity 3→4
  (`wait-value`); lowered to `encodeWaitForEvent:`. Negative fixture: consumer submitted
  and read to completion before the producer exists → stale `f(0)` bytes,
  deterministically. Positive: producer signals, consumer waits the signal value, an
  independent graph runs between them without waiting (witness event values strictly
  monotonic) → `src + 2` guaranteed.
- **Gap 9** (`gfx_pipeline_request_inline_root`): pipeline jets accept the
  `pipeline-request (artifact, root-layout)` carrier; `root-layout` gained field 3 `mode`
  {1 buffer, 2 inline}. Pipeline cache key = sha(sha(artifact) ‖ stride ‖ mode) — library
  cache stays content-address-keyed. Same artifact under two layouts: cold/cold/hit
  witnessed; inline (`setBytes`/`setVertexBytes`) bytes ≡ buffer-root bytes; >4 KiB inline
  window refuses 948.
- **Gap 10** (`gfx_indirect_dispatch_admits_on_device`): step kind 4
  `dispatch-indirect {alloc offset generation threads-x}`; the args window validated like
  any complement (950 stale, 948 misaligned/OOB — both tested), lowered to
  `dispatchThreadgroupsWithIndirectBuffer:`. The fixture is eq.-17-shaped: stage B's
  kernel admits rows where spatial² < temporal² (3 of 8) and GPU-writes the threadgroup
  counts; stage C (waiting B per Gap 8) marks exactly the admitted count.
- **Gap 7** (`foil/shrine/psa-emitter.plan` + `gfx_foil_msl_emitter_law_artifact`): ~200
  lines of wisp law compute the R(3,1,1) antiproduct table — merge-inversion parity +
  metric (−1,1,1,1,0) contraction, antiproduct as `lcomp(gp(rcomp a, rcomp b))` (the
  ACL4.2 adjoint composition computed literally) — plus antireverse signs, decimal
  printing, and the MSL weld through `bar-weld`. The C test execs wisp, cross-checks
  both table checksums against an **independent C derivation** (exact match), decodes
  the 6,325-byte law-emitted artifact from its traced bar, compiles and dispatches it,
  and matches the readback against the independent CPU table-walker at 1e-5 with
  convention-free reality anchors (ct/z/e4 invariant under the 3-4-5 z-rotor; xy-norm
  preserved; point actually rotated to cos 2φ = 0.28, sin 2φ = 0.96). **The C test never
  authors the artifact — the Slice-1 hand-authored-MSL residue is closed.**

**Post-Slice-2 addendum (2026-07-09)** — `gfx_cross_pass_raw_pixel_divergence` closes
Gap 5's last deferral (the negative RAW *pixel* fixture) with the first multi-pass render
chain: pass A renders into target T, pass B samples T through the descriptor heap into
target U. Zero new native mechanics — targets were already `RenderTarget|ShaderRead` and
intern as ordinary textures, so a target handle written into a descriptor window is the
whole bridge; the declared leg is Gap 8's wait-value. Undeclared → B completes before A
is submitted and deterministically samples the stale seed color; declared → B waits A's
signal and samples the produced pixel. 12 GPU tests; suite + layering green.

---

## Slice 3 — The No-API Floor (pre-Rex completeness)

> *Specs: NoGraphicsApi.md (the Aaltonen prototype API — the mechanics target), the
> Aaltonen transcript (barrier/sync reality, per-stage roots), CB8 (memory model),
> CB9 (payload kinds), CB10 (pipeline second relation), CB16 (no per-frame pipeline
> construction), ACL6.2 (linear residual: no contraction without an admitted law),
> GF1.4 (Foundry byte-tag classes fix the texture format set), SSH4/SSH10.2
> (replay classification: Deterministic vs ReDispatchOnly)*
> **Status: 🔲 PLANNED (2026-07-09)**

**Goal**: complete the jet table + carrier vocabulary to Aaltonen's ~150-line prototype
surface, so that when the Rex policy layer arrives it **only authors rows — never a new
jet**. The admission test for every item: irreducible mechanics → jet; derived
carrier/layout/evidence → Foil record; selection/policy → deferred to `.rex`. Policy
stays as literal values in fixtures until then.

**Design decisions settled up front** (the think-harder deltas):

- **`gpu.write` is consume-and-mint, not mutation.** A pointer complement is a linear
  residual (ACL6.2); overwriting bytes another holder's witness attests to is forbidden
  contraction. Lawful shape: wait the session event (write happens-after all submitted
  GPU work — the Producer-side dual of `gpu.read`), memcpy, **bump the generation**,
  witness `(idx, new-gen)`. Every prior complement over the allocation goes stale (950).
  Frames-in-flight is then a *pattern* (N allocations round-robin — Aaltonen's bump
  allocator), never native cleverness.
- **The clear step retires into a `pass-desc` carrier.** Clear-as-first-step (Gap 3) was
  a workaround for having no pass descriptor. The hardware shape is loadOp+clearValue
  per attachment (Aaltonen's GpuRenderPassDesc); a pass-desc record buys MRT, depth, and
  store ops (`DONT_CARE` matters on TBDR) in one stroke. Step kind 2 becomes reserved-refused.
- **Memory scopes shrink to private textures.** On UMA, readback/default are both shared
  storage — `gpu.read` already covers readback. `MTLStorageModePrivate`'s only real win
  here is texture lossless compression → land as private textures uploaded via a copy
  command (replacing sync `replaceRegion`). Buffer scopes defer to the MoltenVK lane.
- **Specialization constants split.** Value constants are deterministic and kill PSO
  permutations → land. **Embedded device addresses** in constants poison content
  identity (runtime address in the pipeline key → non-replayable; SSH ReDispatchOnly)
  and buy one indirection over root-embedded addresses, which already work → deferred
  with reason.
- **Multi-queue deferred with a named hazard**: `encodeSignalEvent` is a plain set, not
  atomic-max. One queue ⇒ encode order = signal order ⇒ the timeline is monotonic; two
  queues on one session timeline can regress the signaled value and break every
  readback wait. Async compute needs per-queue timelines + cross-queue waits — design
  work, not a checkbox. Defer to the present/overlap era.
- **op-83 joins record/replay.** The `er_io_hook` seam covers op-82 only; GPU jets
  bypass it. Handles/generations/event-values are deterministic given call order and
  readback results are bytes, so hooking opset-83 makes GPU-using actors **replayable
  on machines with no Metal device**. Promoted into this slice as its exit criterion's spine.
- **Consumer anchoring prunes speculation**: texture formats fixed by GF1.4 byte-tag
  classes (R8, RGBA8, BGRA8, RGBA16F + D32) — no mips/3D/cube until a consumer names
  them; ICB-MDI and meshlets are mechanics-complete but consumer-gated (same rule that
  held draw-indexed); placement-heap complements (alloc = literal arena offset — the
  no_api interval table and CB8's "one logical heap" made literal) is the right endgame
  but a deep refactor with no consumer-visible gain now → deferred ledger.

### Gap 17 — the main-line port (Phase 0 — numbered last, sequenced FIRST)

> *Size: MEDIUM | Phase: 0 — before any Slice-3 code, so new gaps land on the merged
> base instead of widening the divergence*

This branch and axsys-org/enki `main` are parallel clean-room lineages (same refactor
commit message, different hashes; merge-base `df9bb6c` predates both). `main` has the
bytecode evaluator + threaded dispatch, the actor runtime + `er_io_hook` record/replay
seam, and op-82 evolution — none of which this tree has. **Strategy: re-port, not
git-merge.**

- Verbatim (new files): `pkg/plan/hostcall.{h,c}`, `pkg/host/**`, `foil/shrine/**`,
  `pkg/plan/src/rex.c`, `tests/unit/test_hostcall_gpu.c`, this plan.
- Hand-applied touch-points on main: the op-83 gate at main's three op-82 gate sites
  (eval.c:424, 611, 934); `&pl_ops[fr->op]` → `pl_op_desc(fr->op)` at the descriptor
  derefs (eval.c:1092/1173/1189) and the `pl_io_run/name/argc` accessors (eval.c:92-100);
  **bytecode.c:85 `pl_op_lookup` → `pl_op_lookup_all`** so compiled laws reach op-83
  (baked `pl_nops+i` indices are stable: registration is once at boot, `pl_code` is
  per-process); export op.c's `nat_name_eq` as `pl_nat_name_eq`; `hostcall_f` on
  `pl_thread`; wisp.c wiring (`hostcall_f` follows the rplan gate — main's wisp already
  adopts the scheduler); Makefile `pkg/host` layer + `check-layering` extension.
- The third lineage: shrine-collapse vendors `enki-main`
  (`shrineOS/Projection Engine/substrate/plan/enki-main`) whose `include/enki/host.h`
  carries the C-struct floor for the **present arm and active selection** (present
  witness, platform-surface frame, backend selection/device facts, effective bytemap,
  projection state, watch-poll carriers) consumed by EnkiBridge/`NoApiMetalBackend.m`.
  It is the reference for the present-arm lane exactly as `NoApiMetalBackend.m` was for
  Gaps 0-6 — mechanics awaiting the Foil lift, not a merge source.

*Pass criteria*: (1) all 12 GPU fixtures + the full main suite green on the ported
main tree (Metal hardware); (2) a bytecode-compiled law invoking an op-83 binding
dispatches through the unified descriptor path (new fixture — impossible on this
branch, main-only); (3) `PL_NO_BYTECODE=1` differential run agrees; (4) check-layering
green with the host layer.

### Gap 11 — sync & data-path completeness

> *Size: SMALL–MEDIUM | Phase: A*
> **Status: ✅ DONE (2026-07-09, on op83-port)** — `gm_wait`/`gm_write` + the blit
> graph (`gm_blit_graph`: pipeline 0 selects copy-only encoding, ≤32 steps, no hazard
> facts — copies are encoder-ordered; destination generations minted at encode and
> appended to the witness in step order).  Fixture
> `gfx_write_copy_wait_data_path`: short write → 948; linear write → old complement
> 950, new gen reads the new bytes; buffer→buffer copy + gpu.wait on its signal;
> buffer→texture→buffer round trip byte-identical; never-signaled wait → 948; draw
> step in a blit graph → 952.  14/14 hostcall+gpu fixtures green.

- `gpu.wait` (session, value): CPU-side wait on the session timeline without reading
  bytes (timeout → 947). Unblocks frames-in-flight patterns.
- `gpu.write` (session, heap-pointer, bytes-bar): consume-and-mint per the decision
  above. Refusals: 950 stale, 948 length ≠ window, 942 unknown slot.
- **Copy step kinds** in the command graph: `copy` (buffer→buffer via blit encoder,
  kind 7), `copy-to-texture` (kind 8), `copy-from-texture` (kind 9) — all windows
  validated via `gm_pointer`, destination generation bumped at encode, visibility
  ordered by the graph's signal value. Closes the reserved blit/readback-step enums.

*Pass criteria*: (1) write → stale-950 on the old complement, new-gen complement reads
back the new bytes; (2) a graph copy lands byte-identical data (buffer and texture legs);
(3) `gpu.wait` returns after the signaled value with no readback; (4) suite green.

### Gap 12 — op-83 record/replay

> *Size: MEDIUM | Phase: A (parallel with Gap 11)*
> **Status: ✅ DONE (2026-07-09, on op83-port)** — the op_body seam widened to
> `(opset 82 | 83) && !coord`; `pl_io_opset` joins the public log-identity surface;
> `er_hostcall_hook` (actor.c) records each host call as `ER_EV_HOSTCALL` — args
> content-hashed structurally (nats by bytes, pins by content hash, apps/laws by shape
> recursion, post-`pl_nf`), result stored as the flattened head-0 row (binding name +
> n length-prefixed nat elements, since bindings exceed the 7-byte mote) — and replays
> by rebuilding the row verbatim, **no jet body runs**.  Fixtures:
> `gfx_hostcall_record_replay_substitutes` (records open→arena→alloc→read→close, then
> replays the first four on a fresh runtime — the session was closed live, so a live
> read would refuse 941; the recorded bytes come back instead; cursor == 4) and
> `hostcall_replay_divergence_aborts` (`.signal = SIGABRT`: a divergent binding at the
> same site trips the (actor, binding, args-hash) verification — deviceless).  16/16
> fixtures; full suite + layering + format green.  Baseline audit same day: every
> main-diff hunk intentional, PL_NO_BYTECODE differential 13/13, GC_STRESS 13/13 +
> 28/28.

Extend the direct-effect interception seam to opset-83: recording captures each
hostcall's result row (witness or refusal, including readback/read payload bytes);
replay substitutes results without touching Metal after verifying (actor, binding,
args-hash) — exactly the op-82 discipline. Sessions/allocs/generations/event-values are
deterministic given call order, so the log is complete.

*Pass criteria*: (1) a recorded GPU slice replays bit-identically with the Metal jets
compiled out (or on a 943 no-device machine); (2) divergence (edited args) aborts;
(3) live suite unaffected.

### Gap 13 — the raster floor (the ABI-break stroke)

> *Size: LARGE | Phase: B — one stroke while tests are the only caller, same rationale
> as Gaps 2+3*
> **Status: 🟡 PHASE 1 LANDED (2026-07-09, on op83-port)** — the pass/root/depth
> machinery: `pass-desc` carrier (MRT-capable color attachments with load/store ops +
> clear values, optional depth attachment with f32-bits clear, optional cached
> `ds-state`) replaces the bare target; **the clear step is retired** (952; clear rides
> the pass-desc load action, and steps-executed counts dropped accordingly); split
> chains force Load/Store on intermediates and honor declared ops on first/final;
> per-stage roots land as bare-pointer-or-`pair(vx, px)` with the fragment root at
> fragment buffer 0; `gpu.target` gained the format field {1 bgra8, 5 d32};
> `pipeline-request` field 3 `raster-state (depth-format, reserved)` joins the pipeline
> cache key.  All 13 render call sites migrated; 16/16 fixtures green.
> **Phase 2 (2026-07-09)**: depth occlusion landed (`gfx_depth_occludes_and_dont_care_store`:
> near-blue-then-far-red two-instance draw under ds-state Less+write — blue wins; depth
> store DONT-CARE with correct color; raster-state pipeline carries Depth32Float) and
> **draw-indexed landed** (kind 5 `{index-ptr icount instances u16|u32}`, window validated
> via gm_pointer with 948/952 refusal legs — `gfx_indexed_draw_through_complement`).
> 18/18 fixtures.  **Remaining (phase 3)**: formats R8/RGBA8/RGBA16F, per-target
> writemasks, private/placed textures, cull/topology/MSAA/blend in raster-state,
> two-raster-states cache fixture.

- **`pass-desc` carrier** replacing the bare target arg on `gpu.command-graph`:
  `pass-desc (color-attachments depth-attachment ds-state k)`; each color attachment
  `(target load-op store-op clear-bgra8)`; depth attachment `(target load-op store-op
  clear-depth)`. MRT + depth + store ops; clear step (kind 2) retired to
  reserved-refused.
- **`raster-state` carrier** as `pipeline-request` field 3: topology, cull, MSAA
  sample-count, color formats + writemasks, depth format, optional embedded blend
  (on Metal blend IS pipeline state — a legitimate divergence from the blog's separate
  blend object). Pipeline cache key = sha(lib ‖ stride ‖ mode ‖ raster-hash) — CB10's
  second relation, again.
- **`depth-stencil-state` carrier** → cached `MTLDepthStencilState`, referenced from
  pass-desc (per-pass granularity, per Aaltonen: "configure once per render pass").
- **Per-stage roots**: graph takes `(vx-root, px-root)` heap-pointer-or-0 records;
  fragment root at buffer(0), dheap stays at buffer(1) both stages; passing the same
  record twice reproduces today's shape.
- **`draw-indexed` step** (kind 5): a0 = index-window heap-pointer record, a1 =
  index-count, a2 = instance-count, a3 = index-format {1 u16, 2 u32}; bounds =
  count×size ≤ window, alignment 2/4.
- **Texture formats + private textures**: format arg on `gpu.target`/`gpu.texture-2d`
  drawn from the GF1.4-anchored set (R8, RGBA8, BGRA8, RGBA16F, D32);
  `MTLStorageModePrivate` sampled textures uploaded via the Gap-11 copy-to-texture
  step (replaces sync `replaceRegion` for the private path); placed `:onHeap` where
  the format allows, `useHeap` covers residency.
- **Depth hazard class**: access-class 4 (depth-stencil) joins the hazard-fact
  vocabulary (the blog's `HAZARD_DEPTH_STENCIL`).

*Pass criteria*: (1) a depth-tested indexed two-target scene renders with correct
occlusion and per-target writemasks (pixel-asserted on both targets + depth readback);
(2) same artifact under two raster states → two cached pipelines (cold/cold/hit);
(3) `DONT_CARE` store on a transient depth target leaves color correct; (4) old
clear-step graphs refuse 952; (5) suite green.

### Gap 14 — value specialization constants

> *Size: SMALL | Phase: C*
> **Status: ✅ DONE (2026-07-09, on op83-port)** — `pipeline-request` 4-field variant
> carries a pair-list of `fn-constant (kind {1 u32, 2 f32-bits}, value)` records with
> sequential `[[function_constant(N)]]` ids (the no_api-validated shape: sequential-id
> typed values, portable 1:1 to Vulkan spec constants); constants hash into the
> pipeline cache key; both jets specialize (the Vulkan reference silently drops compute
> constants — we do not); address-bearing constants are structurally impossible
> (values cap at u32; kind 3+ refuses 951; <= 32 constants).  Fixture
> `gfx_spec_constants_specialize_pipeline`: two constant sets = two cold pipelines with
> divergent kernel output (2+tid vs 200+tid), re-created set hits the cache, malformed
> kind refuses.  19/19 fixtures green.

`constants` carrier (bytes-bar + declared layout ref) on `pipeline-request` field 4,
lowered to `MTLFunctionConstantValues`; cache key gains sha(constants-bytes). Dead-code
elimination replaces PSO permutations: one artifact + N constant sets = N pipelines by
content, zero shader-source variants. Embedded-address constants explicitly refused
(951) with the determinism rationale — root-embedded `gpu.device-address` remains the
pointer path.

*Pass criteria*: (1) one artifact, two constant sets → two pipelines, each witnessed
cold then hit; (2) a constant-branched kernel produces the constant-selected bytes;
(3) address-bearing constants refuse; (4) suite green.

### Gap 15 — GPU-driven completions (consumer-gated mechanics)

> *Size: MEDIUM | Phase: C*
> **Status: 🟡 CORE LANDED (2026-07-09)** — `draw-indexed-indirect` (kind 6: index +
> args pointer complements, 20-byte 4-aligned window, u16|u32) lowered to
> `drawIndexedPrimitives:indirectBuffer:`; exercised by the exit-criterion fixture with
> **GPU-written args** (a constants-specialized compute stage writes indexCount/
> instanceCount on device).  Remaining: GPU-written descriptor fixture (adopt the
> no_api set0-sampled/set1-storage split), meshlets.

- `draw-indexed-indirect` (kind 6): args window validated like dispatch-indirect
  (stale 950 / bounds-alignment 948), lowered to
  `drawIndexedPrimitives:indirectBuffer:` — symmetric with Gap 10.
- **GPU-written descriptor heap fixture**: a compute step writes a `MTLResourceID`
  into the dheap argument buffer (it is already plain GPU memory), consumed by a
  waiting render graph under a declared descriptors-class fact — the blog's
  `HAZARD_DESCRIPTORS` exercised end-to-end.
- **Meshlet pipeline + `draw-meshlets` step** (kind 10): Metal object+mesh functions
  (type-selected, one entry each), meshlet data as raw complement windows — no new
  binding surface, per the blog.

*Pass criteria*: (1) admissibility-stage-written indexed-indirect args draw exactly the
admitted primitives; (2) the GPU-minted descriptor samples correctly and an undeclared
descriptors hazard is constructible/refused per CB9; (3) a meshlet scene matches its
vertex-pipeline twin pixel-for-pixel; (4) suite green.

### Gap 16 — deferred ledger (design notes with named triggers)

- **ICB-based MDI** (indirect shader selection + per-draw roots) — trigger: the
  Projection Engine's first GPU-driven scene walk.
- **Multi-queue / async compute** — per-queue `MTLSharedEvent` timelines + cross-queue
  wait-values (the plain-set monotonicity hazard above) — trigger: frame overlap in the
  present arm (CSP-MED.2 frame-rate federation).
- **Placement-heap complements** — alloc = literal (offset,length) in the arena,
  `MTLHeapTypePlacement`; full no_api interval-table fidelity — trigger: the first
  allocator-pressure consumer or the MoltenVK port (where suballocation is mandatory).
- **Embedded-address spec constants** — trigger: a measured indirection cost on the
  root-pointer path plus an SSH10.2 ReDispatchOnly replay classification for the
  affected pipelines.
- **Mips/3D/cube/arrays, buffer memory scopes** — trigger: a consumer byte-tag class
  that names them (Foundry mip-atlases, MoltenVK).
- **Present/surface arm** — remains the PNW10 lane, out of scope here.

**Build order**: **17 first** (the main-line port — Gap 12's `er_io_hook` seam only
exists on main) → 11 ∥ 12 → 13 (the stroke) → 14 → 15. 16 never blocks.

**Exit criterion — ✅ LANDED (2026-07-09, `gfx_no_api_floor_exit_criterion`)**: a
constants-specialized compute admissibility stage GPU-writes the indexed-indirect args;
an MRT (2 attachments) + depth (Less+write, DONT-CARE store) render draws through a u16
index complement and the GPU-written args under a declared wait-value; four pixel
readbacks prove occlusion and independent attachment writes — **recorded live on Metal,
then replayed bit-identically on a fresh runtime whose session was closed live** (every
witness handle/generation/signal + all pixels reproduced; 20 of 21 events consumed).
Original statement: one fixture drives a
depth-tested, indexed, multi-target scene whose textures are private+placed, whose
pipeline variants come from value spec-constants (no shader-source permutations), with
a compute admissibility stage GPU-writing the indexed-indirect args and a GPU-minted
descriptor — recorded live on Metal hardware, then **replayed bit-identically on a
machine with no GPU** through the Gap-12 log. When that passes, Aaltonen's prototype
surface exists as admitted mechanics + Foil carriers, and the Rex policy layer has no
jet left to ask for.

---

## Out of Scope (tracked, not forgotten)

- Store hardening: root revisions/CAS, watch-poll, write-batch witnesses (PLR12 lane)
- Present/surface arm: CAMetalLayer, swapchain images, surface generation rows (PNW10)
- MoltenVK/Vulkan target (no_api clone is the reference when it starts)
- ByteCarrierAdmit/BoundaryAdmit proper — carriers here converge toward TAB's field set, but admission lands with the Rex policy layer
- ~~Readback/blit payload kinds beyond enum reservation~~ → Slice 3 Gap 11 (copy steps) and Gap 13 (draw-indexed); dispatch-indirect landed in Slice 2 Gap 10
