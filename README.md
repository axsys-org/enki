# enki

`enki` is a greenfield C11 library project skeleton. The example library is a
generic dynamic array of `void*` with an injected allocator. The project is
structured to make the build, test, fuzz, format, static-analysis, and coverage
paths reproducible through Nix while keeping the actual compilation logic in a
hand-written Makefile.

## Quickstart

Enter the reproducible shell:

```sh
nix develop
```

With direnv installed, automatic shell entry is:

```sh
direnv allow
```

The Makefile also works without Nix when the required tools are installed:

```sh
make
make test
make BUILD_TYPE=asan test
```

## Nix Builds

```sh
nix build
nix build .#enki
nix build .#enki-debug
nix build .#enki-asan
nix build .#enki-ubsan
nix build .#enki-tsan
nix build .#enki-coverage
```

Each package is derived from the same `mkenki` helper and calls:

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

Run unit and property tests directly:

```sh
make test-unit
make test-property
```

Run tests under each sanitizer:

```sh
make BUILD_TYPE=debug test
make BUILD_TYPE=asan test
make BUILD_TYPE=ubsan test
make BUILD_TYPE=tsan test
```

On macOS, nixpkgs' Criterion runtime currently crashes before user tests run
when it is linked into a TSan executable. For that platform only,
`BUILD_TYPE=tsan make test-unit` runs an equivalent no-framework unit runner
against TSan-instrumented enki objects. Linux TSan checks use the Criterion unit
suite.

Property tests use a fixed seed in CI. Override it locally with:

```sh
THEFT_SEED=0x1234 make test-property
```

Criterion is supplied by Nix or the host system through pkg-config. Theft is
vendored under `tests/property/vendor/theft/` as a tiny deterministic
single-purpose runner, because upstream theft is not reliably packaged in
nixpkgs. FFF is vendored as a single header under `tests/support/fff.h`.

## Fuzzing

Build and run the libFuzzer harness with Clang:

```sh
make CC=clang BUILD_TYPE=asan fuzz FUZZ_ARGS="-max_total_time=60 -error_exitcode=1"
```

The flake app passes arguments through to libFuzzer:

```sh
nix run .#fuzz -- -max_total_time=60 -error_exitcode=1
```

The seed corpus lives in `tests/fuzz/corpus/`.

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

## Formatting And Analysis

```sh
make format
make format-check
make compile-commands
make tidy
nix fmt
```

`make compile-commands` uses Bear to generate `compile_commands.json`.
`make tidy` consumes that database and runs clang-tidy with checks from
`.clang-tidy`. `nix fmt` uses treefmt-nix with clang-format for C and headers,
alejandra for Nix, and mdformat for Markdown.

## Design Choices

Nix provides the reproducible environment and CI entry points, while Make remains
the single source of truth for compilation. That keeps local, non-Nix use simple:
if GCC or Clang, Criterion, lcov, Bear, and the other tools are installed, the
same Make targets work outside the flake. Nix only selects the compiler, build
type, and dependencies before calling Make.

The allocator is injected through function pointers so the library has no global
mutable allocation policy and no hidden allocation path. Production callers can
use `enki_allocator_system()`, tests can inject accounting or failing allocators,
and embedders can route allocations into arenas or host-managed memory.
