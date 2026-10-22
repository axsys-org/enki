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
