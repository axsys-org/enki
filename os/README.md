# enki-os

`enki-os` is a serial-only, single-CPU x86_64 unikernel containing Enki,
PLAN, and Reaver. QEMU loads the Multiboot2 ELF directly; the guest has no
host libc, dynamic linker, disk image, or library OS.

The Reaver source tree is packed into a deterministic read-only ROM during
the build. Snapshots and compiled pins use the legacy in-memory content store,
so bindings survive between forms but disappear at reboot. Filesystem writes
are discarded and unsupported host services raise `unsupported on enki-os`.

## Build and run

Enter the pinned toolchain and build from the repository root:

```sh
nix develop path:./os
make -C os
make -C os check
make -C os run
```

The artifact is `os/build/enki-os.elf`. It carries a Multiboot2 header and an
ELF64 physical-entry note for QEMU's direct `-kernel` loader. The serial prompt accepts one Reaver
form at a time, uses `... ` while delimiters are open, and supports Ctrl-C,
Ctrl-D on an empty prompt, and `:quit`.

Run the boot self-test with `make -C os test-self`, or the self-test plus the
scripted Reaver integration session with `make -C os test`.

## Shrine mode

`make -C os run-shrine` boots the serial-only Shrine profile with 3 GiB of
guest RAM below the 4 GiB identity-map boundary and a 1 GiB volatile PLAN
store. It compiles the Helm/Foil closure,
discovers and mounts the bundled `chat`, `demo`, `life`, and `nenex` apps, and
starts the deterministic single-core actor runtime. The initial prompt is
`sept>`.

Alongside namespace expressions and the existing `:m`, `:c`, `:t`, and `:s`
commands, Shrine accepts synthetic requests through the real HTTP worker:

```text
:http GET /chat
:http GET /nenex
:http GET /ns/app/life
:http POST /post/chat/log who=a&text=hello&back=%2Fchat
```

No TCP traffic is involved. `:q` or Ctrl-D stops the coordinator, prints
`bye`, and exits QEMU successfully. Run the 30-minute cold-boot integration
suite with `make -C os test-shrine`.

## Deliberate limits

The guest uses polling COM1 and has interrupts and SMP disabled. Its page
tables identity-map the first 4 GiB. ROM reads are restricted to paths below
`reaver/src`; writable files, general descriptors, sockets, outbound Fetch,
SMP, and persistence are not provided. Shrine's HTTP interface is an in-guest
actor message seam, not a network listener.
