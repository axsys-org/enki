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

## Deliberate limits

The guest uses polling COM1 and has interrupts and SMP disabled. Its page
tables identity-map the first 4 GiB. ROM reads are restricted to paths below
`reaver/src`; writable files, descriptors, clocks, networking, HTTP, actors,
and op-83 services are not provided.
