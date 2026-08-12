#!/usr/bin/env python3
"""Generate a deterministic, link-time ROM index for enki-os."""

from __future__ import annotations

import hashlib
import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: mkrom.py SOURCE_DIR OUT.S OUT.C", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[1]).resolve()
    asm_path = pathlib.Path(sys.argv[2])
    c_path = pathlib.Path(sys.argv[3])
    if not source.is_dir():
        print(f"enki-os: missing Reaver source tree: {source}", file=sys.stderr)
        return 1

    files = sorted(
        (path for path in source.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(source).as_posix(),
    )

    asm = [".section .rodata.rom,\"a\",@progbits", ".balign 16"]
    c = [
        '#include "os/platform.h"',
        "",
    ]
    entries: list[str] = []
    for index, path in enumerate(files):
        relative = path.relative_to(source).as_posix()
        payload = path.read_bytes()
        stamp = int.from_bytes(hashlib.sha256(payload).digest()[:8], "little")
        stamp &= (1 << 63) - 1
        label = f"os_rom_{index}"
        escaped_path = str(path).replace("\\", "\\\\").replace('"', '\\"')
        asm += [
            f".global {label}_start",
            f"{label}_start:",
            f'.incbin "{escaped_path}"',
            f".global {label}_end",
            f"{label}_end:",
            ".balign 8",
        ]
        c += [
            f"extern const unsigned char {label}_start[];",
            f"extern const unsigned char {label}_end[];",
        ]
        entries.append(
            "  {"
            f".path={json.dumps(relative)}, .data={label}_start, "
            f".size=(size_t){len(payload)}, "
            f".stamp=UINT64_C({stamp})"
            "},"
        )

    c += [
        "",
        "const os_rom_file os_generated_rom[] = {",
        *entries,
        "};",
        f"const size_t os_generated_rom_count = {len(entries)};",
    ]
    asm_path.write_text("\n".join(asm) + "\n")
    c_path.write_text("\n".join(c) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
