#!/usr/bin/env python3
"""Exercise shipped executables, including signed corruptions on Darwin."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile

BIN = Path(sys.argv[1]).resolve()
DARWIN = sys.platform == "darwin"
ENV = dict(os.environ, PL_CODECACHE="0")
ENV.pop("ENKI_BUNDLE_MODE", None)


def run(args, cwd, *, expected=0, data=b"", env=None):
    result = subprocess.run([str(a) for a in args], cwd=cwd, env=env or ENV,
                            input=data, capture_output=True, timeout=60)
    assert result.returncode == expected, (args, result.returncode, result.stderr)
    return result


def ops():
    text = ""
    for family, name, args in [("B", "Seq", "a b"), ("B", "Ifz", "c t e"),
                              ("B", "Dec", "x"), ("B", "Bytes", "x"),
                              ("B", "Ix", "i x"), ("R", "Input", "n"),
                              ("R", "Output", "b"), ("R", "Print", "x")]:
        text += f'(#bind {name} (#pin (#law "{name}" ({name} {args}) ((#pin "{family}") ("{name}" {args})))))\n'
    return text


def build(root, name, program):
    source = root / name
    source.mkdir()
    (source / "main.plan").write_text(ops() + program + '\n((#pin "B") ("Save" prog))\n')
    run([BIN / "wisp", source, "main"], source)
    pin = run([BIN / "scry", "snap", "root"], source).stdout.decode().strip()
    output = root / (name + "-tool")
    run([BIN / "enki", "bundle", "--entry", pin, "-o", output], source)
    verify(output, root)
    return output, pin, source / "snap"


def macho_payload(data):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<8I", data)
    assert magic == 0xFEEDFACF
    offset = 32
    segments = []
    payload = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, offset)
        if cmd == 0x19:
            _, _, name, vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from("<II16sQQQQIIII", data, offset)
            name = name.rstrip(b"\0")
            segments.append(name)
            if name == b"__ENKI":
                assert nsects == 1
                section = offset + 72
                addr, length, start = struct.unpack_from("<QQI", data, section + 32)
                assert addr == vmaddr and start == fileoff and length <= filesize
                page = 16384 if os.uname().machine == "arm64" else 4096
                assert vmsize == filesize and filesize % page == 0
                payload = (start, length, section)
            if name == b"__LINKEDIT":
                assert fileoff + filesize == len(data)
        offset += size
    assert segments[-2:] == [b"__ENKI", b"__LINKEDIT"]
    assert payload is not None
    return payload


def verify(path, cwd):
    if DARWIN:
        run(["/usr/bin/codesign", "--verify", "--strict", path], cwd)
        listing = run(["/usr/bin/otool", "-l", path], cwd).stdout
        assert listing.index(b"segname __ENKI") < listing.index(b"segname __LINKEDIT")
        macho_payload(path.read_bytes())


def main():
    with tempfile.TemporaryDirectory(prefix="enki-bundle-") as tmp:
        root = Path(tmp)
        echo, echo_hash, echo_store = build(root, "echo", '''
(#bind prog (#pin (#law "prog" (prog args)
 (Seq (Print (Ix 0 args)) (Seq (Print 10)
  (Seq (Print (Ix 1 args)) (Seq (Print 10)
   (Seq (Print (Ix 2 args)) 7))))))))''')
        empty = root / "empty"
        empty.mkdir()
        home = root / "home"
        home.mkdir()
        runtime_env = dict(ENV, HOME=str(home), PL_CODECACHE="1", PL_CODECACHE_DIR=str(home / "cache"))
        result = run([echo, "--text-hash", "last"], empty, expected=7, env=runtime_env)
        assert result.stdout == f"{echo}\n--text-hash\nlast".encode()
        assert result.stderr == b"" and list(empty.iterdir()) == [] and list(home.iterdir()) == []
        cat, cat_hash, _ = build(root, "cat", '''
(#bind prog (#pin (#law "cat" (cat args)
 chunk(Input 4096)
 (Ifz (Dec (Bytes chunk)) 23 (Seq (Output chunk) (cat args))))))''')
        data = bytes(range(256)) * 1024 + b"\0\0"
        result = run([cat, "--anything"], empty, data=data, expected=23)
        assert result.stdout == data and result.stderr == b""
        # The payload is the only source of pins when rebundling without --store.
        copy = root / "cat-copy"
        rebundle_env = dict(ENV, ENKI_BUNDLE_MODE="1")
        run([cat, "bundle", "--entry", cat_hash, "-o", copy], empty, env=rebundle_env)
        verify(copy, empty)
        assert run([copy], empty, data=data, expected=23).stdout == data
        large, _, _ = build(root, "large", '(#bind prog (#pin (#law "large" (large args) (Print "' + 'x\n' * 50000 + '"))))\n')
        # Exercise the Unix pipeline and default SIGPIPE disposition.
        for blocked in (False, True):
            # Ignored SIGPIPE is reset by enki; blocked SIGPIPE exercises EPIPE.
            block_pipe = (lambda: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGPIPE})) if blocked else None
            producer = subprocess.Popen([str(large)], cwd=empty, env=ENV,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        restore_signals=False, preexec_fn=block_pipe)
            consumer = subprocess.Popen(["head", "-1"], stdin=producer.stdout, stdout=subprocess.PIPE)
            producer.stdout.close()
            first, _ = consumer.communicate(timeout=60)
            assert consumer.returncode == 0 and first == b"x\n"
            diagnostic = producer.stderr.read()
            expected_status = 0 if blocked else -signal.SIGPIPE
            assert producer.wait(timeout=60) == expected_status and diagnostic == b""
        before = large.stat().st_size
        # Replace the executing, signed binary by rename; also shrink __ENKI.
        run([large, "bundle", "--entry", echo_hash, "--store", echo_store, "-o", large], empty, env=rebundle_env)
        assert large.stat().st_size < before
        verify(large, empty)
        assert run([large, "a", "b"], empty, expected=7).stdout == f"{large}\na\nb".encode()
        invalid, _, _ = build(root, "invalid", '(#bind prog (#pin (#law "invalid" (invalid args) 256)))')
        result = run([invalid], empty, expected=1)
        assert result.stdout == b"" and b"0 to 255" in result.stderr
        # Failed exports must leave an existing output and the source intact.
        sentinel = root / "preserved"
        sentinel.write_bytes(b"keep me")
        run([BIN / "enki", "bundle", "--entry", "00" * 32, "--store", echo_store,
             "-o", sentinel], empty, expected=1)
        assert sentinel.read_bytes() == b"keep me"
        assert list(root.glob("*.enki-*")) == []
        original = cat.read_bytes()
        if DARWIN:
            start, length, section = macho_payload(original)
        else:
            length = struct.unpack_from("<Q", original, len(original) - 44)[0] + 60
            start = len(original) - length
        for index, at in enumerate([start, start + length // 2, start + length - 60,
                                     start + length - 48, start + length - 44,
                                     start + length - 36, start + length - 1]):
            changed = bytearray(original)
            changed[at] ^= 1
            bad = root / f"corrupt-{index}"
            bad.write_bytes(changed)
            bad.chmod(0o755)
            if DARWIN:
                run(["/usr/bin/codesign", "--force", "--sign", "-", bad], empty)
                run(["/usr/bin/codesign", "--verify", "--strict", bad], empty)
            result = run([bad], empty, expected=126)
            assert result.stdout == b"" and len(result.stderr.splitlines()) == 1
        truncated = root / "truncated"
        changed = bytearray(original)
        if DARWIN:
            struct.pack_into("<Q", changed, section + 40, length - 1)
        else:
            del changed[-1:]
        truncated.write_bytes(changed)
        truncated.chmod(0o755)
        if DARWIN:
            run(["/usr/bin/codesign", "--force", "--sign", "-", truncated], empty)
        result = run([truncated], empty, expected=126)
        assert result.stdout == b"" and len(result.stderr.splitlines()) == 1
        assert list(empty.iterdir()) == []
    print("bundle integration: passed")


if __name__ == "__main__":
    main()
