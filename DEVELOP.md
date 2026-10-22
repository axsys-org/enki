# enki

`enki` is a PLAN runtime structured as three packages with a mechanically
enforced dependency direction:

| package | prefix | contents |
| ----------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pkg/axsys` | `ax_` | GC-oblivious systems substrate: arena, vector, string builder, allocator, sha256. Knows nothing about the heap or value representation. |
| `pkg/plan` | `pl_` | The PLAN core: value representation, semispace Cheney heap with safepoint+reserve discipline, the explicit-frame reduction machine (whnf/nf), primops, pins, and the content-addressed store (native arena, Silo, LMDB, or memory). |
| `pkg/enki` | `en_` | Everything above the core: the wisp PLAN-assembly front end (parser, macro expander, module loader) and the `wisp` / `assembler` apps. |

## GC Discipline

The collector runs only inside `pl_gc_reserve`; allocation (`pl_bump` and
every constructor) never collects and hard-asserts previously reserved
headroom. Code measures sizes, reserves, re-fetches values from rooted
locations, then builds inside a no-collect window. The collector scans
exactly the registered root sources — no conservative scanning, no C-stack
scanning. A `GC_STRESS=1 make` build collects on every reserve, turning any
rooting violation into a deterministic failure:

```sh
GC_STRESS=1 make BUILD_TYPE=debug BUILD_DIR=build/stress test
```

## Quickstart

Enter the reproducible shell:

```sh
nix develop
```

With direnv installed, automatic shell entry is:

```sh
direnv allow
```

The Makefile also works without Nix when the required tools are installed
(Clang, matching llvm-profdata, Git, GMP, LMDB, OpenSSL, curl, libsodium, and
BLAKE3). Plain `make` builds PGO with SLJIT under `build/pgo-sljit`; its first
build fetches the pinned SLJIT source and runs PGO training. In `nix develop`,
the pinned source and profiling tools are supplied by Nix.

```sh
make
make test
make BUILD_TYPE=debug test
make BUILD_TYPE=asan test
```

## Nix Builds

```sh
nix build
nix build .#enki
nix build .#enki-release
nix build .#enki-debug
nix build .#enki-asan
nix build .#enki-ubsan
nix build .#enki-tsan
nix build .#enki-coverage
```

The default package, `enki`, `wisp`, and `assembler` use PGO with SLJIT.
`enki-release` uses an ordinary release build without PGO or SLJIT. Explicit
debug, sanitizer, and coverage packages also retain their interpreter builds;
CI additionally runs the release suite with SLJIT. Each package is derived
from the same `mkenki` helper and calls:

```sh
make BUILD_TYPE=<type>
make install BUILD_TYPE=<type> PREFIX=$out
```

Coverage builds use GCC so lcov gets a matching gcov tool. On Linux, debug and
release also use GCC, while sanitizer, tidy, and fuzz builds use Clang. On macOS
the sanitizer builds use Clang for platform compatibility. This gives CI
coverage across both compiler families without a separate compiler matrix.

## Tests

Run everything:

```sh
make test
nix flake check -L
```

- `tests/unit` — Greatest suites for the axsys containers, the plan core
  (value representation, nats, GC movement), the reduction machine
  (laziness boundaries, recursive-let knots, primop strictness order),
  pins/store round trips, the wisp front end, and an end-to-end check that
  `wisp reaver/src/plan reaver` exits zero.
- `tests/property` — theft property tests.
- `tests/fuzz` — libFuzzer targets.

Run tests under each sanitizer:

```sh
make BUILD_TYPE=debug test
make BUILD_TYPE=asan test
make BUILD_TYPE=ubsan test
make BUILD_TYPE=tsan test
```

TSan runs the full Greatest suite plus the focused multithreaded stress tests
in `tests/unit/*_tsan.c`. Each Greatest test process has a 60-second timeout.

Property tests use a fixed seed in CI. Override it locally with:

```sh
THEFT_SEED=0x1234 make test-property
```

Greatest and FFF are vendored as single headers under `tests/support/`. Theft
is vendored under `tests/property/vendor/theft/` as a tiny deterministic
single-purpose runner, because upstream theft is not reliably packaged in
nixpkgs.

## Coverage

Generate local HTML coverage:

```sh
make coverage
```

The report is written to `build/coverage/html/index.html`.

Build the reproducible report:

```sh
nix build .#coverage
nix run .#coverage-report
```

The Nix build writes the report under `result/html/`.

## Clang PGO

Plain `make` builds Clang profile-guided `wisp`, `assembler`, and `scry` with
SLJIT. To regenerate the profile and rebuild explicitly:

```sh
make pgo
```

The target builds instrumented binaries under `build/pgo-generate-sljit`,
runs this training workload from `build/pgo-sljit/run`:

```sh
../../pgo-generate-sljit/bin/wisp --file-root ./reaver/src ./reaver/src/plan reaver main
```

and trains `assembler` and `scry`, then merges the generated profiles into
`build/pgo-sljit/enki.profdata`. Any snapshots created by the workload stay
under `build/pgo-sljit/run/snap`. It then rebuilds the optimized binaries under
`build/pgo-sljit/bin` with `-fprofile-instr-use=build/pgo-sljit/enki.profdata`.
Subsequent `make` runs reuse that profile; use `make pgo` to retrain after
changing the workload or runtime. SLJIT and interpreter-only builds use
separate profile and binary directories.

Useful overrides:

```sh
make pgo PGO_CC=/path/to/clang LLVM_PROFDATA=/path/to/llvm-profdata
make pgo PGO_WORKLOAD="--file-root ./reaver/src ./reaver/src/plan reaver main"
make SLJIT_DIR=                  # PGO interpreter, under build/pgo
make BUILD_TYPE=debug            # debug interpreter, under build/debug
make BUILD_TYPE=release          # release interpreter, under build/release
```

## SPLAN Profiling Zones

RPLAN code can mark explicit spans with the direct op-83 primops
`["ZoneStart", label]`, which returns an opaque handle, and
`["ZoneEnd", handle]`. Handles belong to the logical PLAN thread that created
them; zones may be ended out of nesting order, but duplicate, unknown, and
cross-thread handles are runtime errors.

Export those spans as a Chrome Trace JSON file with either CLI spelling:

```sh
wisp --profile-json trace.json DIR MODULE [FUNCTION ARGS...]
wisp --profile-json=trace.json DIR MODULE [FUNCTION ARGS...]
```

Open the resulting file in `chrome://tracing`. Each logical PLAN thread is a
separate lane, and spans are paused while an actor is yielded or blocked. The
JSON export contains explicit zones only; automatic law attribution remains a
Tracy-only feature.

## Signed 63-bit arithmetic

Op set 83 exposes pure `I63Inc`, `I63Dec`, `I63Neg`, `I63Abs`, `I63Add`,
`I63Sub`, `I63Mul`, `I63Div`, and `I63Mod` primitives. Comparisons `I63Eq`,
`I63Ne`, `I63Lt`, `I63Le`, `I63Gt`, and `I63Ge` return ordinary boolean nats
(`0` or `1`). All operands are strict and must be direct nats; heap nats
and non-nats raise an error. Like other op-83 primitives, these operations
require RPLAN mode.

Signed values use zigzag encoding: nonnegative `n` is stored as `2*n`, and
negative `n` as `-2*n-1`. Thus `0, -1, 1, -2, 2` encode as `0, 1, 2, 3, 4`.
The signed range is `-2^62` through `2^62-1`, and every encoded value fits
in the runtime's existing 63-bit direct-nat representation. For example,
calling op 83 with the row `("I63Add" 13 6)` computes `-7 + 3` and returns
`7`, the encoding of `-4`.

Arithmetic wraps modulo `2^63`. Incrementing the maximum gives the minimum;
negating or taking the absolute value of the minimum returns the minimum.
Division truncates toward zero, and remainder has the dividend's sign:
`-7 / 3 = -2` and `-7 % 3 = -1`. Minimum divided by `-1` wraps to minimum,
with remainder zero. Both division and remainder raise on a zero divisor.

The allocation-free C helpers in `plan/int63.h` provide encoding, decoding,
and arithmetic with the same semantics. `pl_int63_encode` wraps any `int64_t`
input to the signed 63-bit range. Decoding and arithmetic require valid
direct-nat operands; the C division/remainder helpers additionally require
a nonzero divisor. Runtime primitives validate those preconditions.

`I63MulDiv(a, b, divisor, rounding)` computes `a*b/divisor` with an exact
signed 128-bit product, rounds the quotient, and only then wraps to 63 bits.
The first three arguments are zigzag integers; `rounding` is a plain nat:

| Mode | Meaning | `7/2` | `-7/2` |
| --- | --- | --- | --- |
| `0` | Toward zero | `3` | `-3` |
| `1` | Floor, toward negative infinity | `3` | `-4` |
| `2` | Ceiling, toward positive infinity | `4` | `-3` |
| `3` | Nearest, with exact ties going to the even integer | `4` | `-4` |

For ties-to-even, `5/2` and `-5/2` round to `2` and `-2`. Exact integer
quotients are unchanged in every mode. Negative divisors are supported.
For example, the op-83 row `("I63MulDiv" 13 2 4 3)` computes
`roundEven(-7*1/2)` and returns `7`, the encoding of `-4`. A fixed-point
projection `2^40 * 2^30 / 2^35` returns `2^35`, even though the intermediate
product exceeds 63 bits. Using `I63Mul` followed by `I63Div` would wrap the
product prematurely. Final overflow still wraps; this is not saturating or
checked arithmetic. Unknown rounding modes, non-direct arguments, and zero
divisors raise errors. The C helper `pl_int63_muldiv` uses the
`PL_INT63_ROUND_ZERO`, `PL_INT63_ROUND_FLOOR`, `PL_INT63_ROUND_CEIL`,
and `PL_INT63_ROUND_EVEN` enum values and requires
valid operands, a nonzero divisor, and a valid rounding mode.

SLJIT executes all sixteen `I63*` primitives natively for direct-nat operands
in known calls and tail calls, including calls with already-forced operands.
The native path preserves the RPLAN-mode gate, op-83 memoization barrier,
and evaluator fuel accounting. Deferred or invalid operands and zero divisors
resume the interpreter at the original call, as do invalid rounding modes.
`I63MulDiv` uses a full-width multiply and a bounded 64-step division for
products wider than a machine word; smaller products use machine division.
Neither native path allocates or calls C. Unforced primitive thunks remain lazy.

## Formatting And Analysis

```sh
make format
make format-check
make tidy
nix fmt
```

`make tidy` consumes `compile_commands.json` (generate with Bear) and runs
clang-tidy with checks from `.clang-tidy`. `nix fmt` uses treefmt-nix with
clang-format for C and headers, alejandra for Nix, and mdformat for Markdown.

## Debugging Linux builds with Docker

The Docker workflow uses a pinned Nix 2.34.7 image and the same `tools/nix-ci`
runner as GitHub Actions. The default platform is `linux/amd64`, matching the
GitHub Ubuntu runner:

```sh
make linux-check
```

On Apple Silicon, use the native platform for faster iteration:

```sh
make linux-check LINUX_PLATFORM=linux/arm64
```

Checks stage a clean source copy inside the container, excluding Git metadata,
build artifacts, snapshots, and the local `reaver` checkout. This keeps linked
Git worktrees reproducible without modifying the host workspace. The Docker
harness disables Nix's inner syscall filter for cross-architecture emulation;
Docker remains the outer isolation boundary, and native CI keeps the filter.

Open an interactive Linux development shell with the current worktree mounted
read/write at `/src`:

```sh
make linux-shell
# or, on Apple Silicon
make linux-shell LINUX_PLATFORM=linux/arm64
```

Nix downloads are cached in a Docker volume specific to the image version and
architecture. Remove a cache when a clean Nix store is needed:

```sh
docker volume rm enki-nix-2-34-7-linux-amd64
docker volume rm enki-nix-2-34-7-linux-arm64
```

## Native persistence

The default backend borrows the fixed-address persistence approach from
[xocore-tech/PLAN](https://github.com/xocore-tech/PLAN), with a standalone native
image rather than an LMDB object index. Its format is a local, trusted ABI
image, not a portable interchange or untrusted input format. Native objects
retain the canonical Silo SHA-256 identities; encoding is streamed into the
hash and physical addresses never contribute to identity.

| Profile | Fixed base | Reserved capacity |
| --- | --- | --- |
| macOS arm64, Linux x86-64 | `0x7a0000000000` | 256 GiB |
| Linux arm64 | `0x100000000` | 256 GiB |
| Linux arm64 with sanitizers | `0x100000000` | 8 GiB |

Capacity is virtual address space, not file size or resident memory. The entire
window is reserved without overwriting existing mappings. File pages replace
only that reservation. Allocation and protection use 64 KiB granules on 4 KiB
or 16 KiB hosts; files grow incrementally. Two superblocks occupy the first two
granules. Each has its own SHA-256 checksum, generation, committed end, root
address/hash, object/cache index roots, journal head, and next PIN identifier,
as well as format/layout version, architecture, endian marker, base, and
capacity. A different profile is incompatible, including the smaller Linux
arm64 sanitizer profile.

Each PIN record contains its canonical PIN and non-PIN body graph, preserving
internal sharing. Nested PIN references point to earlier native records. Small
records pack within granules; large records start on a granule boundary. PIN
cell 6 contains an immutable identifier. `pl_pin_code_slot` resolves that ID to
an atomic slot in a separate, lazily backed anonymous mapping. Transient PINs
keep their inline slot. Compiled code is attached on demand; replaced code
survives until store teardown so suspended frames remain valid. Actor payloads
and other transient shared values continue to use the anonymous store region.

Object and per-store compiler-cache indexes are append-only Patricia trees.
Ice stages new object extents, updates a private mutable index overlay, seals
object granules read-only, and only then resolves source proxies. Save writes
each final changed index node once, reuses immutable committed subtrees, stages
a journal record for a changed root, seals pages, synchronizes appended data,
writes the inactive superblock, and synchronizes again (`F_FULLFSYNC` on macOS,
`fsync` on Linux). Initial file
creation also synchronizes the containing directory. Saving the same root
checkpoints staged objects/cache mappings without duplicating history. There
is no GC, compaction, relocation, migration, or multi-store address allocation.
Each Ice/Save boundary starts subsequent allocation on a fresh granule, so
many tiny Ice operations have a measurable padding cost.

A writer holds `arena.writer.lock` for its lifetime. Readers acquire a separate
`arena.commit.lock` while capturing a checkpoint and mapping extensions. They
can retain old pointers because published records are immutable. Startup
selects the newest valid checkpoint, checks its metadata/index heads/root,
and ignores abandoned staging. Object body checksums are reserved for explicit
verification, so opening or loading a large extent only faults in its header.
Startup does not recursively deserialize Silo or
scan/register all PINs. A writer truncates abandoned staging before appending.
No valid checkpoint in an existing file is an error. Failures before
superblock publication can be retried: completed index records are memoized
and reused, and subsequent inserts invalidate only changed overlay paths.
Ambiguous publication failures disable writes until reopen. The overlay is
released after a successful checkpoint or on close. Store teardown requires
all its evaluator threads to have
stopped and all store pointers to have been discarded.

The optional machine-wide LMDB compiler/Memo cache remains independent.
Compiler rows cross that boundary as canonical Silo streams. Import first
stages native objects; the next Save checkpoints their local cache mapping.
Cache absence, contention, and malformed rows remain nonfatal.

`pl_store_objects` and `pl_store_object_info` provide backend-neutral inspector
access. `scry` supports history, checkout, graph navigation, object attribution,
`has-root`, `usage`, and `verify`. Native object lengths include record headers.
Usage separates indexed object bytes, current index nodes, history records,
and remaining overhead (superblocks, padding, cache records, obsolete COW
nodes, and abandoned records included in a later commit). Full verification
checks every committed record checksum, unique PIN IDs, live object layouts,
local cycles, pointer bounds, dependency closure, canonical hashes, index
routing, cache references, and history. Run it explicitly before using an
image whose integrity is in doubt; ordinary startup intentionally avoids a
full scrub.

### Native tests and benchmarks

`tests/unit/test_plan_native.c` uses fresh exec'd subprocesses for reopen,
writer exclusion, live inspectors, and crash injection. It also covers exact
addresses/collisions, sharing, large extents, canonical hashes, capacity and
profile rejection, staged writes, retry/poison behavior, code-slot concurrency,
compiler replacement, and global-cache export/import. The private fault hook
covers growth, object staging, data synchronization, and both sides of
superblock publication. A fresh empty image is also a valid checkpoint, so a
torn first Save may recover that empty checkpoint.

```sh
make BUILD_TYPE=debug build/debug/tests/unit/test_plan_native
build/debug/tests/unit/test_plan_native
make BUILD_TYPE=asan test
make BUILD_TYPE=ubsan test
make BUILD_TYPE=tsan test
make GC_STRESS=1 BUILD_TYPE=debug BUILD_DIR=build/stress test
make BUILD_TYPE=release build/release/tests/perf/bench_native
mkdir /tmp/native-bench /tmp/silo-bench
build/release/tests/perf/bench_native native create /tmp/native-bench 10000
build/release/tests/perf/bench_native native read /tmp/native-bench
build/release/tests/perf/bench_native silo create /tmp/silo-bench 10000
build/release/tests/perf/bench_native silo read /tmp/silo-bench
```

The benchmark reports Save latency, restart/open time, first useful evaluation,
sparse access, evaluator throughput, anonymous store bytes, registered PINs,
anonymous resident memory (when available), and peak RSS. Native reads assert that startup registers nothing and loading
this graph registers only its root. Run each read in a new process; this is a
process-cold measurement with a potentially warm filesystem cache, not a
controlled cold-disk benchmark. On macOS arm64, five release-mode reads of a 10,000-PIN image measured a
median 1.87 ms to first evaluation versus Silo's 43.53 ms. Anonymous
resident memory was 3.52 MB versus 33.85 MB; anonymous store-object
allocations were 96 bytes versus 27.4 MB. The single Save batch took
117 ms versus 17 ms for Silo. Evaluator throughput was similar
(54.5 versus 54.1 million identity applications/second). That initial version
traded additional COW index writes and checksumming for fast direct restart.
Repeat measurements on the deployment filesystem before drawing throughput
conclusions.

Validation of this implementation covered the full debug, ASan, UBSan, and
GC-stress suites on macOS arm64, Linux arm64, and emulated Linux x86-64;
macOS also passed the release suite. The full TSan suite passed on macOS
arm64 and Linux arm64 with fixed mapping enabled. Linux x86-64 TSan compiled,
but exited before `main` in the emulated container with an incompatible memory
layout / denied ASLR change. That profile still needs a native x86-64 TSan
runner. Linux skipped the optional Haskell-oracle comparison because its
binary was unavailable. The initial native suite contained 20 tests, including actual
concurrent code-slot claims and cache attachment. Linux leak detection also
exposed an existing empty staged-cache lookup allocation leak, now fixed.

### Baseline Save profiling (2026-10-19)

The arena implementation is committed as `f88adb1`. Profiling used macOS
arm64 release builds and the existing `bench_native` workload: distinct small
PINs plus one enclosing root. Each measurement created a fresh directory and
process; five interleaved repetitions were run per backend and size. These
numbers characterize a batch of many small objects, not every application
snapshot or cold-disk behavior.

| Child PINs | Native Save | Silo default | Silo `PL_SILO_FSYNC=full` |
| --- | ---: | ---: | ---: |
| 1,000 | 16.2 ms | 6.9 ms | 9.7 ms |
| 10,000 | 119.5 ms | 17.0 ms | 20.4 ms |
| 50,000 | 662.6 ms | 73.0 ms | 75.4 ms |

These are uninstrumented medians. Silo defaults to `F_BARRIERFSYNC` for its
pack on macOS, followed by LMDB's commit; native uses two `F_FULLFSYNC` calls.
Forcing full pack sync in Silo adds only about 3 ms at 10,000 PINs, so this
policy difference accounts for little of the observed slowdown.

An isolated copy of the committed sources was instrumented with nested
monotonic timers and call counters. At 10,000 child PINs (10,001 objects), the
instrumented Save median was 137.3 ms; instrumentation increased runtime, so
the following timings are attribution evidence rather than baseline latency.
Exclusive categories do not include their child scopes:

| Work | Calls | Median exclusive time |
| --- | ---: | ---: |
| Verify native record checksums | 372,176 | 62.4 ms |
| Generate native record/superblock checksums | 141,732 | 24.3 ms |
| Two durable file synchronizations | 2 | 10.3 ms |
| Canonical Silo streaming hash | 10,001 | 3.7 ms |
| Extend file | 243 | 2.6 ms |
| Synchronize mapped pages | 1 | 2.0 ms |
| Map extensions | 243 | 0.8 ms |

The two checksum categories consume about 63% of the instrumented Save.
`pl_native_stage` first traverses the index to look for an existing hash;
`ninsert` descends again to find the split, then again to record its path.
Every visit calls `nnode`, which verifies the record's SHA-256, including
nodes this same writer just created. Insertion then copies and hashes every
ancestor on the path. An independent 1 ms macOS `sample` capture, with timers
disabled, corroborated the checksum hotspot and showed OpenSSL's one-shot
`SHA256` path spending substantial time in provider lookup, context allocation,
initialization, and cleanup around the small hashes.

This batch writes **131,729 index nodes**, although its final tree contains
only **20,001**. The 111,728 superseded nodes occupy 12,513,536 bytes. The full
image is 16,187,392 bytes: 1,200,152 object bytes, 14,753,648 total index bytes,
120 journal bytes, and 233,472 bytes of headers/padding. Thus about 77% of this
first checkpoint is already obsolete index metadata. This is repeated
copy-on-write work within one unpublished transaction, not useful history.

Diagnostic variants were built only in a temporary source copy. Five runs
per variant, with timing instrumentation disabled, gave these medians:

| Diagnostic variant | 10,000 PINs | 50,000 PINs |
| --- | ---: | ---: |
| Same-build control | 123.3 ms | 722.8 ms |
| Trust checksums of newly written staging index nodes | 61.5 ms | 334.2 ms |
| Reuse the native SHA-256 context | 54.6 ms | 301.6 ms |
| Both | 40.7 ms | 224.8 ms |

The control is from the same diagnostic executable, not the baseline run
above. Trusting staging retained header/bounds checks and continued checking
committed index nodes. Context reuse retained SHA-256 and all on-disk
checksums. Neither variant changed synchronization. One output per variant
and size (eight images) passed the unchanged production full-image verifier.
These are diagnostic prototypes, not production patches: in particular, the
prototype digest context is single-threaded and needs proper ownership before
use in a runtime with concurrent inspectors.

The profiling suggested this optimization order (the first two are implemented
below):

1. Reuse digest state with explicit thread/store ownership, and avoid repeated
   verification of nodes created by the current writer. Keep committed-image
   validation and explicit full verification intact.
1. Maintain the working Patricia index in process-local memory and serialize
   each final changed node once at checkpoint. Reuse immutable committed
   subtrees, preserve append-only publication, and keep object pages read-only.
   This addresses the within-transaction write amplification that remains
   after the diagnostic improvements.
1. Re-measure before changing growth granularity: `ftruncate` and `mmap`
   together cost only about 3.4 ms in this case. Keep the required durable
   flushes; they are not the primary bottleneck for this workload.

The local raw measurements, sample stack report, record counts, and temporary
instrumentation scripts are retained under `build/save-profile/` (ignored
build artifacts). Those diagnostic experiments left the runtime unchanged; the implementation
below applies their findings.

### Implemented Save optimizations

The native writer now reuses a store-owned OpenSSL digest context under the
existing `save_mu` lock. Read-only inspectors have their own context, and all
contexts are released on store close. SHA-256 identities, record checksums,
full-image verification, and durability barriers are unchanged.

New index nodes stay in a private mutable Patricia overlay. They need no
checksum verification on traversal because they have not been serialized;
committed nodes still take the validating disk path. Insertion records its
path in one descent, modifies existing overlay ancestors, and copies committed
ancestors only when necessary. Save serializes the final overlay in child-first
order and hashes each emitted record once. On retry, unchanged emitted records
are reused, while mutations invalidate the affected paths. No committed page
is rewritten. The v1 format is unchanged, and old and new binaries were tested
reading each other's images.

Five interleaved release runs using the original committed benchmark executable
and the optimized one produced these macOS arm64 medians:

| Child PINs | Original native Save | Optimized native Save | Silo Save | Original image | Optimized image |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 20.5 ms | 9.5 ms | 8.5 ms | 1.38 MB | 0.52 MB |
| 10,000 | 151.4 ms | 27.7 ms | 21.8 ms | 16.19 MB | 3.60 MB |
| 50,000 | 859.9 ms | 139.0 ms | 99.2 ms | 93.13 MB | 17.43 MB |

These runs have different absolute timings from the earlier profiling session;
compare the interleaved controls within this table. The 10,000-PIN batch now
emits exactly 20,001 index nodes instead of 131,729. The additional transient
cost is a 120-byte overlay entry per working node, plus allocator spare
capacity; the overlay is discarded after successful Save. This memory is not
needed on reopen. Raw measurements are in `build/save-profile/optimized.json`.

The native regression suite now has 25 tests. Added coverage counts actual
index records (including obsolete ones), requires zero index records after
Ice, verifies partial-emission reuse, exercises new inserts and cache
replacement after a synchronized but unpublished checkpoint, checks committed
checksum validation while an overlay exists, and runs concurrent writer/store
and read-only inspector queries through the shared digest context. Crash
injection now includes index serialization.

Validation for this optimization passed the full debug, release, ASan, UBSan,
TSan, and GC-stress suites on macOS arm64; full debug, ASan, UBSan, and TSan
suites on Linux arm64; and full debug, ASan, and UBSan suites on emulated
Linux x86-64. Linux also passed the native suite with GCC UBSan. The optional
Haskell-oracle CLI comparison was skipped on Linux. Native x86-64 TSan still
requires the non-emulated runner noted above.

## SLJIT backend

The experimental SLJIT backend is included and enabled by default in PGO
builds. Make fetches commit `39c508d1b8fbae332d60c7450db20236913089da` into
`build/deps/`; Nix pins the same source in `flake.lock`. Set `SLJIT_DIR=` to
build without it, or supply a checkout to enable it in another build type.
For an explicit release build with SLJIT:

```sh
git clone https://github.com/zherczeg/sljit.git build/sljit-src
git -C build/sljit-src checkout 39c508d1b8fbae332d60c7450db20236913089da
make BUILD_TYPE=release SLJIT_DIR="$PWD/build/sljit-src" \
  build/release-sljit/bin/wisp \
  build/release-sljit/tests/unit/test_plan_eval \
  build/release-sljit/tests/unit/test_plan_thread \
  build/release-sljit/tests/perf/bench_jit
```

Use the normal development environment for Enki's other dependencies. The
`-sljit` build-directory suffix is automatic unless `BUILD_DIR` is supplied.
The external SLJIT source is compiled separately from the project's warning
policy. On Apple Silicon its allocator uses MAP_JIT and per-thread write
protection; other platforms use its W^X allocator. Only macOS arm64 has been
measured for this experiment.

Native execution is enabled when `PL_JIT` is unset or set to `1`.
`PL_JIT=0` disables it and keeps the bytecode interpreter; other explicit
values also disable it. Configuration is read once on the first bytecode decode.
`PL_JIT_THRESHOLD` is the number of eligible interpreter encounters per code
object before compilation (default 32, minimum 1). `PL_JIT_STATS=1` reports
compiled/failed code objects, native entries, compilation nanoseconds and
machine-code bytes at process exit. Leave statistics off for performance
comparisons: its atomic entry counter changes the measured workload.

The backend prepares a private dispatch overlay after bytecode validation and
optimization. It instruments runs of at least three supported instructions,
and lazily compiles code objects of at most 8192 bytecode slots. It supports
variable/literal/slot loads, already-evaluated Force, branches, jumps, local
tail blocks with up to 32 arguments, the bounded construction batches below,
and fast paths for 82 explicitly allowlisted opset-66 primitives:

- Arithmetic: Inc, Dec, Add, Sub, Mul, Div, Mod, Lsh, Rsh.
- Bits/bytes: Test, Nib, Load8, LoadVar, Store8, Set, Clear, Bex,
  Trunc8/16/32/64, Trunc, Bits, Bytes.
- Comparisons and selection: Nil, Truth, Eq, Ne, Lt, Le, Gt, Ge, Cmp,
  Or, Nor, And, If, Ifz, Case, Case2–Case16, Seq, Seq2, Seq3.
- Inspection/projection: Nat, Type, IsPin, IsLaw, IsApp, IsNat, Arity,
  Name, Body, Unpin, Hd, Sz, Last, Ix, Ix0–Ix7.
- Terminal direct-nat cases of Force, DeepSeq, and Equal.

The same lowering handles CALL_KNOWN/CALL_READY and known primitive tail
calls, including the decoder's numeric opcodes. MK_THK KNOWN uses it only
for the existing total, cheap-eagerness specializations. Arithmetic handles
direct nats and WHNF non-nat coercion; big nats, overflow, division by zero,
deferred strict operands, and deferred selected results resume the original
instruction. Lazy branches check only the selected result. Unpin proxies use
the runtime's acquire-load resolution. Force/DeepSeq/Equal retain runtime
normalization and structural comparison for nonterminal values.

The top operand stays in a register across straight-line sequences. Branch
joins materialize it; resumes inside a sequence reload it through an entry
stub. Native code makes no C calls and cannot collect, raise, or suspend. It
can allocate a complete construction batch after checking heap and stack
space. It spills the value-stack cursor, cached operand and fuel before
returning the bytecode offset of the pending instruction. That instruction
executes once in the interpreter, even when its overlay says JIT. Completed
primitive tail calls return a separate sentinel; the evaluator pops the
execution frame and delivers the already-evaluated result without primitive
dispatch or argument relocation. Guards preserve exact fuel checkpoints and
exception/GC behavior without native stack maps.

This is a fragment JIT. General calls (including recursive law tail calls),
returns, allocating primitives such as Pin/Law/Row/Rep/Slice/Weld/Init/Up,
application/control primitives such as Elim/Sap/Try/Memo, and storage,
tracing, compiler installation, and other non-allowlisted operations still
use the evaluator. Executable memory and entry tables belong to the specific
`pl_code` version and are freed with it; they are never persisted.
Compilation is serialized and native entries are published with release/acquire
atomics so shared code can be entered by multiple evaluator threads.

Correctness checks (repeat with debug/GC_STRESS and UBSan build directories):

```sh
env -u PL_JIT -u PL_JIT_THRESHOLD build/release-sljit/tests/unit/test_plan_eval
PL_JIT=1 PL_JIT_THRESHOLD=1 build/release-sljit/tests/unit/test_plan_eval
PL_JIT=1 PL_JIT_THRESHOLD=1 build/release-sljit/tests/unit/test_plan_thread
PL_JIT=0 build/release-sljit/tests/unit/test_plan_eval
```

The evaluator tests retain canonical decoded opcodes for their existing
assertions. Additional JIT tests compare loop results and suspended frame/PC
sequences against the interpreter at fuel quanta 2–33, collect at every yield,
exercise environment and stack argument storage, overflow and lazy-operand
fallbacks through the existing numeric suite, stack growth, invalid branch
exceptions, and concurrent compilation/publication. The JIT-specific tests
assert that machine code was actually compiled. The primitive coverage test
also checks the native exit PC, so a compiled wrapper that falls back at the
primitive does not count as native coverage. Primitive differential tests
cover ordinary/tail/speculative forms, coercion and overflow, lazy selected
and unselected branches, exceptions, and identical yield/frame sequences at
fuel quanta 2–16 with a collection at every yield.

Benchmarks:

```sh
PL_JIT=0 build/release-sljit/tests/perf/bench_jit 1000000
PL_JIT=1 build/release-sljit/tests/perf/bench_jit 1000000
python3 tools/bench-sljit.py build/release-sljit/bin/wisp \
  --output build/sljit-results-new
```

The microbenchmark sums 1..N using a local tail loop and checks every result.
Sample 0 includes warmup/compilation. The workload runner copies `reaver/src`
into a new output directory and runs the existing P6 benchmark bodies with
checksums, one warmup and five timed batches per workload. It runs JIT off/on/
on/off in separate processes with separate native snapshots and persistent
compiler caching disabled. JSON records, source/binary fingerprints, and logs
stay in the output directory. Reported workload medians exclude the first
sample per workload; process wall time also includes startup, source loading,
compiler installation, warmup, and snapshots.

### Initial results (2026-10-22)

Measured on macOS 26.3 arm64, Apple Clang 21, `-O3 -DNDEBUG -flto`, using
SLJIT commit `39c508d1b8fbae332d60c7450db20236913089da`. Both modes used the
same executable. Each median contains ten post-warmup samples from two
processes; the workload batches below contain the listed iteration counts.

| Workload | Iterations per batch | Interpreter median | JIT median | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Local tail loop (sum 1..3,000,000) | 1 | 25.075 ms | 10.572 ms | 2.37x |
| nfib | 20 | 91.151 ms | 77.609 ms | 1.17x |
| listwork | 3 | 54.582 ms | 55.788 ms | 0.98x |
| rowwork | 3 | 175.857 ms | 178.452 ms | 0.99x |
| sortwork | 3 | 1947.321 ms | 2024.952 ms | 0.96x |
| bstwork | 3 | 673.966 ms | 654.048 ms | 1.03x |
| compwork | 300 | 583.269 ms | 614.423 ms | 0.95x |

All workload checksums passed. Whole-process times were 73.12/79.65 seconds
with JIT off and 75.52/82.18 seconds with JIT on. These exploratory runs were
not on an isolated CPU; process and sample variation make the small workload
differences inconclusive. There is no demonstrated overall startup benefit.
A separate instrumented P6/nfib smoke run compiled 236 code objects, reported
6.13 ms of backend compilation time and 2,278,112 emitted machine-code bytes
(excluding allocator granularity, overlays, entry tables and compiler scratch).
That counter-enabled run was excluded from the timing table.

The strong local-loop improvement shows that native regions are useful when
execution remains inside them. This subset is not a general speedup for the
realistic workload mix. Use `PL_JIT=0` to compare against the interpreter. The
next useful comparison is call and return lowering, plus identifying which
short regions pay more in crossing costs than they save. Backend compilation
time alone does not explain the whole-program results.

Validation: the release evaluator suite passed 97 tests / 17,025 assertions
with compilation threshold 1, including concurrent publication. The same
suite passed with GC stress and UBSan; the existing thread suite passed with
JIT enabled. A build with no SLJIT dependency also passed the evaluator suite.
Generated machine code is not instrumented by UBSan, so its checks complement
the interpreter/native differential tests rather than replacing them.

## Fixed-shape construction batches

The decoder groups straight-line pushes and at least two fixed-size APP or
THKE constructions into bounded plans (64 instructions, 4096 cells). Entry
reserves stack and heap space once; the ordinary constructor handlers consume
the reserved cells. Calls, forces, branches, entry targets, and eager primitive
shortcuts terminate a plan. Canonical bytecode and literal ownership are
unchanged. Collection happens before construction with inputs still rooted;
there are no new fuel or suspension points inside a batch.

Set `PL_BATCH=0` to disable planning. `tests/perf/bench_build.c` isolates two
linked row allocations per loop iteration. On Apple Silicon, reservation-only
batching was approximately neutral in this microbenchmark (about 15–16 ms per
million iterations); removing reserve checks alone is not a demonstrated speedup.
The plans also provide bounded construction metadata for native lowering.
`PL_CACHE_STATS` reports reserve calls, and the existing workload runner accepts
`--toggle PL_BATCH` for same-binary comparisons with `PL_JIT=0`.

SLJIT lowers each plan to native header/payload stores and a single heap-cursor
publication. Stack room, variable bounds, and heap headroom are checked before
construction; a failed guard returns to the batch's original interpreter entry.
There are no native entry points in the middle of construction. Literal loads
still go through canonical decoded bytecode, and newly constructed objects can
be shared by later steps. Known primitive thunks retain their resolved index
and NOUPD flag; potentially raising primitives remain lazy.

`PL_GC_STRESS` and `PL_CACHE_STATS` builds deliberately use the C batch path to
retain forced collections and allocation counters. The native batch tests
compare graph sharing, fuel checkpoints, and moving roots against both the
canonical and batched interpreters. They also exercise exact heap fits and
transactional fallback for insufficient heap/stack space and invalid variables.

An Apple Silicon release build, using the same binary with `PL_JIT=1` and
`PL_BATCH=0,1,1,0`, produced these medians (one warmup and two timed samples
per process, four measured samples per mode). All 72 workload checksums passed.
This is a local comparison, not a cross-platform or PGO result; the first
control process was slower than the second.

| Workload | Batching off | Batching on | Speedup |
| --- | ---: | ---: | ---: |
| nfib | 83.308 ms | 72.750 ms | 1.145x |
| listwork | 53.925 ms | 50.635 ms | 1.065x |
| rowwork | 165.620 ms | 151.628 ms | 1.092x |
| sortwork | 2016.872 ms | 1782.283 ms | 1.132x |
| bstwork | 672.172 ms | 608.589 ms | 1.104x |
| compwork | 596.682 ms | 540.727 ms | 1.103x |

After those runs, the isolated two-row loop took 8.8–9.0 ms with SLJIT but no
batching, 4.0–4.3 ms with native batches, and 13.9–14.6 ms with both disabled
(per million iterations, five timed samples after warmup in each process).
The earlier reservation-only measurements above were taken separately.
