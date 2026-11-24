# enki

`enki` is a PLAN runtime.

## Running

```sh
# compiling
nix build
# testing
nix flake check -L
# full CI check (tests followed by the default package build)
make nix-ci
```

`nix build`, `make`, and `x/boot` default to a Clang PGO build with SLJIT
enabled. Local binaries are under `build/pgo-sljit/bin`; the first build runs
the PGO training workload. Set `PL_JIT=0` to disable native execution at runtime.
Use `make BUILD_TYPE=debug` for a debug build or `make SLJIT_DIR=` for PGO
without SLJIT. See [DEVELOP.md](DEVELOP.md#clang-pgo) for build details.

## Entering a reaver dev environment

```sh
## start, or resume
x/boot
## reboot (publish a fresh root, retaining history)
x/boot --reset
```

## Native snapshots

`wisp` stores snapshots in `snap/arena.dat`. The arena reopens at a fixed virtual
address and executes directly from mapped objects. `Ice` stages objects;
`Save` durably publishes the root and its dependencies. Closing without Save
loses staged writes. History is append-only, including after `x/boot --reset`.

Existing Silo snapshots are rejected. Keep the old directory as a backup and
start with a fresh `snap` directory; there is no automatic migration or deletion.
`--text-hash` and the library's memory/Silo constructors remain available.

Inspect a snapshot while its writer is running:

```sh
build/debug/bin/scry snap log
build/debug/bin/scry snap usage
build/debug/bin/scry snap verify
```

Only one native store may be open in each process, and only one process may
write a store. An occupied address window is an error; images never relocate.
See [DEVELOP.md](DEVELOP.md#native-persistence) for the format and validation tools.

## Executable bundles

`enki bundle --entry <64-hex SHA-256 pin hash> -o ./tool` exports a pin
from `./snap`; `--store DIR` selects another native or Silo store. The payload
is a canonical Silo row of binary bars containing the reachable pins' canonical
Silo streams in dependency order, with the entry last. Pin identities retain
Silo's SHA-256 hashing. Boot verifies the entire closure and serves it from a
read-only memory store without opening snapshots or a persistent compiler cache.

Linux uses `[ELF][payload][60-byte trailer]`. macOS uses the exact blob in
`__ENKI,__payload`, immediately before `__LINKEDIT`, with page-aligned segment
sizes and an ad-hoc signature (thin arm64/x86_64 only). The little-endian trailer
contains `ENKIBNDL`, version 1, flags 0, payload length, entry hash, and CRC32C of
its first 56 bytes. **Running `strip` or `objcopy` on a bundled Linux executable
discards its payload.** Bundle only after stripping the base executable.

The entry receives a row of string naturals containing all arguments, including
`argv[0]`. Existing RPLAN `Input`/`Output`/`Warn`/`Print` operations provide IO;
the result must be a natural from 0–255 and becomes the exit status. Invalid
results and execution errors exit 1; malformed bundles exit 126 with one stderr
line. SIGPIPE has its default disposition; an observed EPIPE exits quietly.
Use `ENKI_BUNDLE_MODE=1 ./tool bundle --entry HASH -o ./new-tool` to rebundle from
its embedded store, or add `--store DIR` for another entry. Outputs are published
by atomic rename after writing, flushing, marking executable, and (on macOS)
signing a temporary file in the destination directory.
