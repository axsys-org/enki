#!/usr/bin/env python3
"""Black-box serial tests for the freestanding enki-os kernel."""

from __future__ import annotations

import argparse
import os
import selectors
import subprocess
import sys
import time


READY = b"enki> "


class Guest:
    def __init__(self, qemu: str, kernel: str, *, command_line: str = "") -> None:
        command = [
            qemu,
            "-machine", "q35",
            "-cpu", "max",
            "-smp", "1",
            "-m", "1024M",
            "-kernel", kernel,
            "-display", "none",
            "-serial", "stdio",
            "-monitor", "none",
            "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        ]
        if command_line:
            command += ["-append", command_line]
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        assert self.process.stdout is not None
        os.set_blocking(self.process.stdout.fileno(), False)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.transcript = bytearray()

    def read_until(self, marker: bytes, timeout: float) -> bytes:
        start = len(self.transcript)
        deadline = time.monotonic() + timeout
        while marker not in self.transcript[start:]:
            if self.process.poll() is not None:
                self._drain()
                raise RuntimeError(
                    f"guest exited with {self.process.returncode} before {marker!r}"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"timed out waiting for {marker!r}; serial tail: "
                    f"{bytes(self.transcript[-4000:])!r}"
                )
            for key, _ in self.selector.select(min(remaining, 1.0)):
                chunk = os.read(key.fd, 65536)
                if chunk:
                    self.transcript.extend(chunk)
        return bytes(self.transcript[start:])

    def _drain(self) -> None:
        assert self.process.stdout is not None
        while True:
            try:
                chunk = os.read(self.process.stdout.fileno(), 65536)
            except BlockingIOError:
                return
            if not chunk:
                return
            self.transcript.extend(chunk)

    def send(self, data: bytes) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)


def checked_window(guest: Guest, form: bytes, expected: bytes, timeout: float = 45) -> None:
    guest.send(form + b"\n")
    window = guest.read_until(READY, timeout)
    if expected not in window:
        raise AssertionError(f"expected {expected!r} after {form!r}, got {window[-2000:]!r}")


def selftest(qemu: str, kernel: str) -> None:
    guest = Guest(qemu, kernel, command_line="selftest")
    try:
        guest.read_until(b"ENKI_OS_SELFTEST_OK", 60)
        status = guest.process.wait(timeout=5)
        if status != 1:  # isa-debug-exit encodes guest code 0 as host status 1
            raise AssertionError(f"unexpected QEMU debug-exit status {status}")
        data = bytes(guest.transcript)
        if b"FATAL:" in data or b"abort" in data:
            raise AssertionError(data[-4000:])
    finally:
        guest.stop()


def integration(qemu: str, kernel: str) -> None:
    guest = Guest(qemu, kernel)
    try:
        # A cold native Wisp assembly plus std import can take several minutes
        # under QEMU TCG (especially on a non-x86 host).
        guest.read_until(READY, 600)
        checked_window(guest, b"(Add 1 2)", b"3")
        checked_window(
            guest,
            b"(Add 340282366920938463463374607431768211456 1)",
            b"340282366920938463463374607431768211457",
        )
        checked_window(
            guest,
            b"(Mul 18446744073709551616 18446744073709551616)",
            b"340282366920938463463374607431768211456",
        )
        checked_window(
            guest,
            b"(Div 340282366920938463463374607431768211456 18446744073709551616)",
            b"18446744073709551616",
        )
        checked_window(
            guest,
            b'(Gt (Stamp "reaver/test-basic.rvr") 0)',
            b"1",
        )
        checked_window(guest, b"(#bind os-test-value 41)", b"41")
        checked_window(guest, b"(Add os-test-value 1)", b"42")

        guest.send(b"(Add\n")
        guest.read_until(b"... ", 10)
        guest.send(b" 1 2)\n")
        window = guest.read_until(READY, 30)
        if b"3" not in window:
            raise AssertionError(window[-2000:])

        guest.send(b")\n")
        guest.read_until(READY, 30)
        checked_window(guest, b"(Add os-test-value 2)", b"43")

        guest.send(b"(Add 1\n")
        guest.read_until(b"... ", 10)
        guest.send(b"\x03")
        guest.read_until(READY, 10)
        checked_window(guest, b"(Add os-test-value 3)", b"44")

        for _ in range(16):
            checked_window(guest, b"(Add os-test-value 1)", b"42")

        guest.send(b":quit\n")
        guest.read_until(b"enki-os: halted", 10)
        status = guest.process.wait(timeout=5)
        if status != 1:
            raise AssertionError(f"unexpected QEMU debug-exit status {status}")
    finally:
        if os.environ.get("ENKI_OS_TEST_TRANSCRIPT"):
            sys.stderr.buffer.write(guest.transcript)
        guest.stop()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--kernel", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--selftest", action="store_true")
    mode.add_argument("--integration", action="store_true")
    args = parser.parse_args()
    try:
        if args.selftest:
            selftest(args.qemu, args.kernel)
        else:
            integration(args.qemu, args.kernel)
    except Exception as error:
        print(f"enki-os test failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
