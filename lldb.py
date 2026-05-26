# enki_lldb.py
#
# LLDB helpers for the enki_value tagged-pointer representation from enki/value.h.
#
# Load in LLDB:
#   (lldb) command script import /absolute/path/to/enki_lldb.py
#
# Then try:
#   (lldb) enki x_v
#   (lldb) enki -d 4 app_v
#   (lldb) enki-obj some_enki_app_ptr
#   (lldb) enki-header x_v
#   (lldb) enki-tag some_raw_ptr
#   (lldb) enki-untag x_v
#   (lldb) enki-layout
#
# The module also registers a type summary for variables whose debug type is `enki_value`.

from __future__ import annotations

import shlex
import traceback
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

try:
    import lldb  # type: ignore
except Exception:  # lets `python -m py_compile enki_lldb.py` work outside LLDB
    lldb = None  # type: ignore


TAG_BIT = 1 << 63
VALUE_MASK = (1 << 64) - 1
PTR_MASK = VALUE_MASK ^ TAG_BIT

TAGS = {
    0: "PIN",
    1: "LAW",
    2: "APP",
    3: "BIG_NAT",
    4: "FWD",
    5: "CONT",
}

STATES = {
    0: "WHNF",
    1: "NF",
    2: "THUNK",
}


class EnkiError(Exception):
    pass


def _is_valid(obj) -> bool:
    try:
        return bool(obj) and obj.IsValid()
    except Exception:
        return bool(obj)


def _hex(v: int, width: int = 16) -> str:
    return f"0x{(v & VALUE_MASK):0{width}x}"


def _hex_addr(v: int, addr_size: int = 8) -> str:
    width = max(2, addr_size * 2)
    return f"0x{v:0{width}x}"


def is_enki_ptr(v: int) -> bool:
    return bool(v & TAG_BIT)


def enki_to_ptr(v: int) -> int:
    return v & PTR_MASK


def ptr_to_enki(p: int) -> int:
    return (p | TAG_BIT) & VALUE_MASK


def _parse_int_literal(expr: str) -> Optional[int]:
    s = expr.strip()
    if not s:
        return None

    # Accept common C integer suffixes for literal-only usage.
    suffixes = ("ull", "ULL", "ul", "UL", "u", "U", "ll", "LL", "l", "L")
    changed = True
    while changed:
        changed = False
        for suf in suffixes:
            if s.endswith(suf) and len(s) > len(suf):
                s = s[: -len(suf)]
                changed = True
                break

    try:
        return int(s, 0) & VALUE_MASK
    except Exception:
        return None


def _selected_frame(debugger):
    target = debugger.GetSelectedTarget() if debugger is not None else None
    if not _is_valid(target):
        return None
    process = target.GetProcess()
    if not _is_valid(process):
        return None
    thread = process.GetSelectedThread()
    if not _is_valid(thread):
        return None
    frame = thread.GetSelectedFrame()
    if not _is_valid(frame):
        return None
    return frame


def eval_u64(debugger, expr: str) -> int:
    lit = _parse_int_literal(expr)
    if lit is not None:
        return lit

    if lldb is None:
        raise EnkiError("not running inside LLDB and expression is not an integer literal")

    frame = _selected_frame(debugger)
    if frame is None:
        raise EnkiError("no selected frame/process; pass a numeric literal or run under a live target")

    # Keep expression evaluation intentionally boring; we only need a scalar/pointer value.
    val = frame.EvaluateExpression(expr)
    err = val.GetError()
    if not _is_valid(val) or (err is not None and err.Fail()):
        msg = err.GetCString() if err is not None else "invalid expression"
        raise EnkiError(f"could not evaluate {expr!r}: {msg}")

    return val.GetValueAsUnsigned(0) & VALUE_MASK


@dataclass
class StructDesc:
    name: str
    size: int
    fields: Dict[str, Tuple[int, int]]  # field -> (offset bytes, byte size)
    from_debug_info: bool = False

    def off(self, field: str, default: Optional[int] = None) -> int:
        if field in self.fields:
            return self.fields[field][0]
        if default is not None:
            return default
        raise EnkiError(f"missing layout field {self.name}.{field}")

    def width(self, field: str, default: int) -> int:
        if field in self.fields:
            return max(1, self.fields[field][1])
        return default


class Layout:
    """Target-dependent object layout.

    Uses DWARF type info when available; otherwise falls back to normal LP64 C layout for
    the header in the question. The fallback is deliberately explicit because padding in
    obj_header matters.
    """

    def __init__(self, target):
        self.target = target
        self.addr_size = 8
        self.size_t_size = 8
        self.enki_value_size = 8
        self.mp_limb_size = 8
        self.byte_order = "little"

        if _is_valid(target):
            try:
                self.addr_size = int(target.GetAddressByteSize()) or 8
            except Exception:
                pass
            self.byte_order = self._target_byte_order(target)
            self.size_t_size = self._sizeof_first(["size_t"], self.addr_size)
            self.enki_value_size = self._sizeof_first(["enki_value", "uint64_t"], 8)
            self.mp_limb_size = self._sizeof_first(["mp_limb_t", "unsigned long"], self.addr_size)

        self.obj_header = self._struct_or_fallback(
            ["obj_header", "enki_value_header"],
            StructDesc(
                "obj_header",
                24,
                {
                    "kind_b": (0, 1),
                    "size_s": (8, self.size_t_size),
                    "state_b": (16, 1),
                },
                False,
            ),
        )

        h = self.obj_header.size
        ev = self.enki_value_size
        ss = self.size_t_size
        limb = self.mp_limb_size

        self.pin = self._struct_or_fallback(
            ["enki_pin"],
            StructDesc(
                "enki_pin",
                h + 32 + ev + ss,
                {
                    "h": (0, h),
                    "hash_b": (h, 32),
                    "inner_v": (h + 32, ev),
                    "n_subpins_s": (h + 32 + ev, ss),
                    "subpins_v": (h + 32 + ev + ss, ev),
                },
                False,
            ),
        )

        # LP64 fallback: obj_header is 24 bytes; uint32_t arity at 24; padding to 32.
        law_arity_off = h
        law_name_off = _align_up(law_arity_off + 4, ev)
        self.law = self._struct_or_fallback(
            ["enki_law"],
            StructDesc(
                "enki_law",
                law_name_off + ev * 2 + ss * 2,
                {
                    "h": (0, h),
                    "arity_s": (law_arity_off, 4),
                    "name_v": (law_name_off, ev),
                    "body_v": (law_name_off + ev, ev),
                    "bc_len_s": (law_name_off + ev * 2, ss),
                    "n_const_s": (law_name_off + ev * 2 + ss, ss),
                    "data_b": (law_name_off + ev * 2 + ss * 2, 1),
                },
                False,
            ),
        )

        self.nat = self._struct_or_fallback(
            ["enki_nat"],
            StructDesc(
                "enki_nat",
                h + ss,
                {
                    "h": (0, h),
                    "n_limbs_s": (h, ss),
                    "limbs": (h + ss, limb),
                },
                False,
            ),
        )

        self.app = self._struct_or_fallback(
            ["enki_app"],
            StructDesc(
                "enki_app",
                h + ev + ss,
                {
                    "h": (0, h),
                    "fn_v": (h, ev),
                    "n_args_s": (h + ev, ss),
                    "args_v": (h + ev + ss, ev),
                },
                False,
            ),
        )

        self.cont = self._struct_or_fallback(
            ["enki_cont"],
            StructDesc(
                "enki_cont",
                h + ss,
                {
                    "h": (0, h),
                    "n_args_s": (h, ss),
                    "args_v": (h + ss, ev),
                },
                False,
            ),
        )

    def _target_byte_order(self, target) -> str:
        try:
            bo = target.GetByteOrder()
            if lldb is not None and bo == lldb.eByteOrderBig:
                return "big"
        except Exception:
            pass
        return "little"

    def _sizeof_first(self, names: Sequence[str], default: int) -> int:
        for name in names:
            t = self._find_type(name)
            if _is_valid(t):
                try:
                    n = int(t.GetByteSize())
                    if n > 0:
                        return n
                except Exception:
                    pass
        return default

    def _find_type(self, name: str):
        if not _is_valid(self.target):
            return None
        try:
            t = self.target.FindFirstType(name)
            if _is_valid(t):
                return t
        except Exception:
            pass
        return None

    def _struct_or_fallback(self, names: Sequence[str], fallback: StructDesc) -> StructDesc:
        for name in names:
            t = self._find_type(name)
            if not _is_valid(t):
                continue
            fields: Dict[str, Tuple[int, int]] = {}
            try:
                n = int(t.GetNumberOfFields())
                for i in range(n):
                    f = t.GetFieldAtIndex(i)
                    if not _is_valid(f):
                        continue
                    fname = f.GetName()
                    if not fname:
                        continue
                    off = None
                    if hasattr(f, "GetOffsetInBytes"):
                        try:
                            off = int(f.GetOffsetInBytes())
                        except Exception:
                            off = None
                    if off is None and hasattr(f, "GetOffsetInBits"):
                        try:
                            off = int(f.GetOffsetInBits()) // 8
                        except Exception:
                            off = None
                    if off is None:
                        continue
                    fsize = 0
                    try:
                        ftype = f.GetType()
                        if _is_valid(ftype):
                            fsize = int(ftype.GetByteSize())
                    except Exception:
                        pass
                    fields[fname] = (off, fsize)
                size = int(t.GetByteSize())
                if size <= 0:
                    size = fallback.size
                if fields:
                    return StructDesc(name, size, fields, True)
            except Exception:
                continue
        return fallback

    def all_descs(self) -> List[StructDesc]:
        return [self.obj_header, self.pin, self.law, self.nat, self.app, self.cont]


def _align_up(n: int, align: int) -> int:
    if align <= 1:
        return n
    return (n + align - 1) & ~(align - 1)


@dataclass
class Header:
    kind: int
    size: int
    state: int

    @property
    def kind_name(self) -> str:
        return TAGS.get(self.kind, f"UNKNOWN({self.kind})")

    @property
    def state_name(self) -> str:
        return STATES.get(self.state, f"UNKNOWN({self.state})")


class Memory:
    def __init__(self, target):
        if not _is_valid(target):
            raise EnkiError("no valid LLDB target")
        process = target.GetProcess()
        if not _is_valid(process):
            raise EnkiError("no valid LLDB process; run or attach first")
        self.target = target
        self.process = process
        self.layout = Layout(target)

    def read_bytes(self, addr: int, size: int) -> bytes:
        if size < 0:
            raise EnkiError(f"negative read size {size}")
        if size == 0:
            return b""
        err = lldb.SBError() if lldb is not None else None
        data = self.process.ReadMemory(addr, size, err)
        if err is not None and err.Fail():
            raise EnkiError(f"could not read {size} byte(s) at {_hex_addr(addr, self.layout.addr_size)}: {err.GetCString()}")
        if isinstance(data, str):
            data = data.encode("latin1")
        data = bytes(data)
        if len(data) != size:
            raise EnkiError(f"short read at {_hex_addr(addr, self.layout.addr_size)}: wanted {size}, got {len(data)}")
        return data

    def read_uint(self, addr: int, size: int) -> int:
        data = self.read_bytes(addr, size)
        return int.from_bytes(data, self.layout.byte_order, signed=False)

    def read_u8(self, addr: int) -> int:
        return self.read_uint(addr, 1)

    def read_size_t(self, addr: int) -> int:
        return self.read_uint(addr, self.layout.size_t_size)

    def read_enki_value(self, addr: int) -> int:
        return self.read_uint(addr, self.layout.enki_value_size) & VALUE_MASK

    def read_header(self, ptr: int) -> Header:
        h = self.layout.obj_header
        kind = self.read_u8(ptr + h.off("kind_b"))
        size = self.read_uint(ptr + h.off("size_s"), h.width("size_s", self.layout.size_t_size))
        state = self.read_u8(ptr + h.off("state_b"))
        return Header(kind=kind, size=size, state=state)


class EnkiInspector:
    def __init__(self, target):
        self.mem = Memory(target)
        self.layout = self.mem.layout

    def brief(self, v: int) -> str:
        v &= VALUE_MASK
        if not is_enki_ptr(v):
            return f"imm {_hex(v)} ({v})"
        ptr = enki_to_ptr(v)
        try:
            h = self.mem.read_header(ptr)
            return (
                f"{h.kind_name}/{h.state_name} "
                f"tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} size={h.size}"
            )
        except Exception as e:
            return f"ptr tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} unreadable: {e}"

    def header_lines(self, v_or_ptr: int, *, raw_ptr: bool = False) -> List[str]:
        if raw_ptr:
            ptr = enki_to_ptr(v_or_ptr) if is_enki_ptr(v_or_ptr) else v_or_ptr
            tagged = ptr_to_enki(ptr)
        else:
            if not is_enki_ptr(v_or_ptr):
                return [f"not a tagged enki pointer: {_hex(v_or_ptr)} ({v_or_ptr})"]
            tagged = v_or_ptr & VALUE_MASK
            ptr = enki_to_ptr(tagged)
        h = self.mem.read_header(ptr)
        return [
            f"tagged: {_hex(tagged)}",
            f"ptr:    {_hex_addr(ptr, self.layout.addr_size)}",
            f"kind:   {h.kind} ({h.kind_name})",
            f"state:  {h.state} ({h.state_name})",
            f"size:   {h.size}",
        ]

    def describe(
        self,
        v: int,
        *,
        depth: int = 2,
        max_items: int = 16,
        max_bytes: int = 64,
        max_nat_limbs: int = 256,
        _indent: int = 0,
        _seen: Optional[set] = None,
    ) -> List[str]:
        v &= VALUE_MASK
        if _seen is None:
            _seen = set()
        ind = "  " * _indent

        if not is_enki_ptr(v):
            return [f"{ind}imm {_hex(v)} ({v})"]

        ptr = enki_to_ptr(v)
        try:
            h = self.mem.read_header(ptr)
        except Exception as e:
            return [f"{ind}ptr tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} unreadable: {e}"]

        head = (
            f"{ind}{h.kind_name}/{h.state_name} "
            f"tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} size={h.size}"
        )
        if depth <= 0:
            return [head]
        if ptr in _seen:
            return [head + "  ↩ seen"]
        _seen.add(ptr)

        try:
            if h.kind == 0:
                lines = [head]
                lines.extend(self._describe_pin(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if h.kind == 1:
                lines = [head]
                lines.extend(self._describe_law(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if h.kind == 2:
                lines = [head]
                lines.extend(self._describe_app(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if h.kind == 3:
                lines = [head]
                lines.extend(self._describe_nat(ptr, max_nat_limbs, _indent + 1))
                return lines
            if h.kind == 4:
                lines = [head]
                lines.extend(self._describe_fwd(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if h.kind == 5:
                lines = [head]
                lines.extend(self._describe_cont(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            return [head, f"{ind}  unknown kind; raw payload starts at +{self.layout.obj_header.size}"]
        except Exception as e:
            return [head, f"{ind}  error while decoding payload: {e}"]

    def _describe_child_value(
        self,
        label: str,
        value: int,
        depth: int,
        max_items: int,
        max_bytes: int,
        max_nat_limbs: int,
        indent: int,
        seen: set,
    ) -> List[str]:
        ind = "  " * indent
        lines = [f"{ind}{label}: {self.brief(value)}"]
        if depth > 1 and is_enki_ptr(value):
            lines.extend(
                self.describe(
                    value,
                    depth=depth - 1,
                    max_items=max_items,
                    max_bytes=max_bytes,
                    max_nat_limbs=max_nat_limbs,
                    _indent=indent + 1,
                    _seen=seen,
                )
            )
        return lines

    def _describe_pin(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.pin
        ind = "  " * indent
        hash_b = self.mem.read_bytes(ptr + d.off("hash_b"), min(32, d.width("hash_b", 32)))[:32]
        inner = self.mem.read_enki_value(ptr + d.off("inner_v"))
        n_sub = self.mem.read_uint(ptr + d.off("n_subpins_s"), d.width("n_subpins_s", self.layout.size_t_size))
        sub_base = ptr + d.off("subpins_v")

        lines = [f"{ind}hash: {hash_b.hex()}"]
        lines.extend(self._describe_child_value("inner_v", inner, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        lines.append(f"{ind}n_subpins_s: {n_sub}")
        for i in range(min(n_sub, max_items)):
            sv = self.mem.read_enki_value(sub_base + i * self.layout.enki_value_size)
            lines.extend(self._describe_child_value(f"subpins_v[{i}]", sv, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if n_sub > max_items:
            lines.append(f"{ind}  ... {n_sub - max_items} more subpin(s) not shown")
        return lines

    def _describe_law(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.law
        ind = "  " * indent
        arity = self.mem.read_uint(ptr + d.off("arity_s"), d.width("arity_s", 4))
        name = self.mem.read_enki_value(ptr + d.off("name_v"))
        body = self.mem.read_enki_value(ptr + d.off("body_v"))
        bc_len = self.mem.read_uint(ptr + d.off("bc_len_s"), d.width("bc_len_s", self.layout.size_t_size))
        n_const = self.mem.read_uint(ptr + d.off("n_const_s"), d.width("n_const_s", self.layout.size_t_size))
        data = ptr + d.off("data_b")
        const_base = data
        bc_base = data + n_const * self.layout.enki_value_size

        lines = [
            f"{ind}arity_s:   {arity}",
            f"{ind}bc_len_s:  {bc_len}",
            f"{ind}n_const_s: {n_const}",
        ]
        lines.extend(self._describe_child_value("name_v", name, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        lines.extend(self._describe_child_value("body_v", body, depth, max_items, max_bytes, max_nat_limbs, indent, seen))

        lines.append(f"{ind}const_table_v @ {_hex_addr(const_base, self.layout.addr_size)}")
        for i in range(min(n_const, max_items)):
            cv = self.mem.read_enki_value(const_base + i * self.layout.enki_value_size)
            lines.extend(self._describe_child_value(f"const[{i}]", cv, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if n_const > max_items:
            lines.append(f"{ind}  ... {n_const - max_items} more const(s) not shown")

        n = min(bc_len, max_bytes)
        bc = self.mem.read_bytes(bc_base, n) if n else b""
        lines.append(f"{ind}bc_b @ {_hex_addr(bc_base, self.layout.addr_size)}")
        lines.append(f"{ind}bc_hex[{n}/{bc_len}]: {_format_bytes(bc)}")
        if bc_len > max_bytes:
            lines.append(f"{ind}  ... {bc_len - max_bytes} byte(s) not shown")
        return lines

    def _describe_nat(self, ptr: int, max_nat_limbs: int, indent: int) -> List[str]:
        d = self.layout.nat
        ind = "  " * indent
        n_limbs = self.mem.read_uint(ptr + d.off("n_limbs_s"), d.width("n_limbs_s", self.layout.size_t_size))
        limb_base = ptr + d.off("limbs")
        limb_size = self.layout.mp_limb_size
        limb_bits = limb_size * 8
        n_show = min(n_limbs, max_nat_limbs)
        limbs = [self.mem.read_uint(limb_base + i * limb_size, limb_size) for i in range(n_show)]

        lines = [f"{ind}n_limbs_s: {n_limbs}", f"{ind}limbs @ {_hex_addr(limb_base, self.layout.addr_size)}"]
        if n_limbs <= max_nat_limbs:
            value = 0
            for i, limb in enumerate(limbs):
                value |= limb << (i * limb_bits)
            lines.append(f"{ind}value_dec: {value}")
            lines.append(f"{ind}value_hex: {hex(value)}")
        else:
            lines.append(f"{ind}value_dec: <not computed; {n_limbs} limbs exceeds max {max_nat_limbs}>")
        if limbs:
            rendered = ", ".join(f"{i}:{_limb_hex(x, limb_size)}" for i, x in enumerate(limbs[: min(len(limbs), 16)]))
            lines.append(f"{ind}limbs[least-significant first]: {rendered}")
            if n_show > 16:
                lines.append(f"{ind}  ... {n_show - 16} more displayed limb(s) omitted from this line")
        else:
            lines.append(f"{ind}limbs: []")
        if n_limbs > max_nat_limbs:
            lines.append(f"{ind}  ... {n_limbs - max_nat_limbs} limb(s) not read")
        return lines

    def _describe_app(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.app
        ind = "  " * indent
        fn = self.mem.read_enki_value(ptr + d.off("fn_v"))
        n_args = self.mem.read_uint(ptr + d.off("n_args_s"), d.width("n_args_s", self.layout.size_t_size))
        args_base = ptr + d.off("args_v")
        lines = []
        lines.extend(self._describe_child_value("fn_v", fn, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        lines.append(f"{ind}n_args_s: {n_args}")
        for i in range(min(n_args, max_items)):
            av = self.mem.read_enki_value(args_base + i * self.layout.enki_value_size)
            lines.extend(self._describe_child_value(f"args_v[{i}]", av, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if n_args > max_items:
            lines.append(f"{ind}  ... {n_args - max_items} more arg(s) not shown")
        return lines

    def _describe_cont(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.cont
        ind = "  " * indent
        n_args = self.mem.read_uint(ptr + d.off("n_args_s"), d.width("n_args_s", self.layout.size_t_size))
        args_base = ptr + d.off("args_v")
        lines = [f"{ind}n_args_s: {n_args}"]
        for i in range(min(n_args, max_items)):
            av = self.mem.read_enki_value(args_base + i * self.layout.enki_value_size)
            lines.extend(self._describe_child_value(f"args_v[{i}]", av, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if n_args > max_items:
            lines.append(f"{ind}  ... {n_args - max_items} more arg(s) not shown")
        return lines

    def _describe_fwd(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        # The posted header declares the tag but not the concrete forwarding object shape.
        # Treat the first payload word as an enki_value-sized forwarding target, which is
        # the usual moving-GC shape. If your runtime uses another layout, update this only.
        ind = "  " * indent
        payload = ptr + self.layout.obj_header.size
        try:
            target = self.mem.read_enki_value(payload)
        except Exception as e:
            return [f"{ind}fwd payload unreadable at {_hex_addr(payload, self.layout.addr_size)}: {e}"]
        lines = [f"{ind}fwd_payload[0] @ {_hex_addr(payload, self.layout.addr_size)}: {self.brief(target)}"]
        if depth > 1 and is_enki_ptr(target):
            lines.extend(
                self.describe(
                    target,
                    depth=depth - 1,
                    max_items=max_items,
                    max_bytes=max_bytes,
                    max_nat_limbs=max_nat_limbs,
                    _indent=indent + 1,
                    _seen=seen,
                )
            )
        return lines


def _limb_hex(x: int, limb_size: int) -> str:
    return f"0x{x:0{limb_size * 2}x}"


def _format_bytes(data: bytes) -> str:
    if not data:
        return ""
    return " ".join(f"{b:02x}" for b in data)


def _get_inspector(debugger) -> EnkiInspector:
    if debugger is None:
        raise EnkiError("not running inside LLDB")
    target = debugger.GetSelectedTarget()
    if not _is_valid(target):
        raise EnkiError("no selected target")
    return EnkiInspector(target)


def _append_lines(result, lines: Iterable[str]) -> None:
    text = "\n".join(lines)
    if hasattr(result, "AppendMessage"):
        result.AppendMessage(text)
    else:
        print(text)


def _set_error(result, msg: str) -> None:
    if hasattr(result, "SetError"):
        result.SetError(msg)
    else:
        print(msg)


def _parse_common(command: str) -> Tuple[Dict[str, int], str]:
    tokens = shlex.split(command)
    opts = {"depth": 2, "max_items": 16, "max_bytes": 64, "max_nat_limbs": 256}
    expr_tokens: List[str] = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t in ("-d", "--depth"):
            i += 1
            if i >= len(tokens):
                raise EnkiError("missing value after --depth")
            opts["depth"] = int(tokens[i], 0)
        elif t in ("-n", "--max-items"):
            i += 1
            if i >= len(tokens):
                raise EnkiError("missing value after --max-items")
            opts["max_items"] = int(tokens[i], 0)
        elif t in ("-b", "--max-bytes"):
            i += 1
            if i >= len(tokens):
                raise EnkiError("missing value after --max-bytes")
            opts["max_bytes"] = int(tokens[i], 0)
        elif t in ("-l", "--max-limbs"):
            i += 1
            if i >= len(tokens):
                raise EnkiError("missing value after --max-limbs")
            opts["max_nat_limbs"] = int(tokens[i], 0)
        elif t == "--":
            expr_tokens = tokens[i + 1 :]
            break
        else:
            expr_tokens = tokens[i:]
            break
        i += 1

    expr = " ".join(expr_tokens).strip()
    if not expr:
        raise EnkiError("missing expression")
    return opts, expr


def _guarded(fn):
    def wrapper(debugger, command, result, internal_dict):
        try:
            return fn(debugger, command, result, internal_dict)
        except EnkiError as e:
            _set_error(result, str(e))
        except Exception:
            _set_error(result, traceback.format_exc())

    return wrapper


@_guarded
def enki_cmd(debugger, command, result, internal_dict):
    """Pretty-print an enki_value expression."""
    opts, expr = _parse_common(command)
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    _append_lines(
        result,
        ins.describe(
            v,
            depth=opts["depth"],
            max_items=opts["max_items"],
            max_bytes=opts["max_bytes"],
            max_nat_limbs=opts["max_nat_limbs"],
        ),
    )


@_guarded
def enki_obj_cmd(debugger, command, result, internal_dict):
    """Pretty-print a raw enki heap object pointer, tagging it temporarily."""
    opts, expr = _parse_common(command)
    raw = eval_u64(debugger, expr)
    ptr = enki_to_ptr(raw) if is_enki_ptr(raw) else raw
    v = ptr_to_enki(ptr)
    ins = _get_inspector(debugger)
    _append_lines(
        result,
        ins.describe(
            v,
            depth=opts["depth"],
            max_items=opts["max_items"],
            max_bytes=opts["max_bytes"],
            max_nat_limbs=opts["max_nat_limbs"],
        ),
    )


@_guarded
def enki_header_cmd(debugger, command, result, internal_dict):
    """Print the obj_header for a tagged enki_value."""
    opts, expr = _parse_common(command)
    del opts
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    _append_lines(result, ins.header_lines(v, raw_ptr=False))


@_guarded
def enki_obj_header_cmd(debugger, command, result, internal_dict):
    """Print the obj_header for a raw heap object pointer."""
    opts, expr = _parse_common(command)
    del opts
    ptr = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    _append_lines(result, ins.header_lines(ptr, raw_ptr=True))


@_guarded
def enki_tag_cmd(debugger, command, result, internal_dict):
    """Apply PTR_TO_ENKI to a pointer expression/literal."""
    opts, expr = _parse_common(command)
    del opts
    p = eval_u64(debugger, expr)
    tagged = ptr_to_enki(p)
    lines = [
        f"ptr:    {_hex_addr(p & PTR_MASK)}",
        f"tagged: {_hex(tagged)}",
        f"lldb expr: (enki_value)((uintptr_t){expr} | (1ULL << 63))",
    ]
    _append_lines(result, lines)


@_guarded
def enki_untag_cmd(debugger, command, result, internal_dict):
    """Apply ENKI_TO_PTR to an enki_value expression/literal."""
    opts, expr = _parse_common(command)
    del opts
    v = eval_u64(debugger, expr)
    ptr = enki_to_ptr(v)
    lines = [
        f"value:  {_hex(v)}",
        f"is_ptr: {1 if is_enki_ptr(v) else 0}",
        f"ptr:    {_hex_addr(ptr)}",
        f"lldb expr: (void*)((uintptr_t){expr} & ~(1ULL << 63))",
    ]
    _append_lines(result, lines)


@_guarded
def enki_nat_cmd(debugger, command, result, internal_dict):
    """Decode an enki_nat / BIG_NAT value as decimal, hex, and limbs."""
    opts, expr = _parse_common(command)
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    if not is_enki_ptr(v):
        _append_lines(result, [f"not a BIG_NAT heap pointer; immediate {_hex(v)} ({v})"])
        return
    ptr = enki_to_ptr(v)
    h = ins.mem.read_header(ptr)
    if h.kind != 3:
        _append_lines(result, [f"warning: kind is {h.kind_name}, not BIG_NAT"] + ins.describe(v, depth=0))
        return
    lines = ins.describe(v, depth=1, max_nat_limbs=opts["max_nat_limbs"])
    _append_lines(result, lines)


@_guarded
def enki_law_cmd(debugger, command, result, internal_dict):
    """Decode an enki_law, including const table and bytecode prefix."""
    opts, expr = _parse_common(command)
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    if not is_enki_ptr(v):
        _append_lines(result, [f"not a LAW heap pointer; immediate {_hex(v)} ({v})"])
        return
    ptr = enki_to_ptr(v)
    h = ins.mem.read_header(ptr)
    if h.kind != 1:
        _append_lines(result, [f"warning: kind is {h.kind_name}, not LAW"] + ins.describe(v, depth=0))
        return
    _append_lines(
        result,
        ins.describe(
            v,
            depth=opts["depth"],
            max_items=opts["max_items"],
            max_bytes=opts["max_bytes"],
            max_nat_limbs=opts["max_nat_limbs"],
        ),
    )


@_guarded
def enki_layout_cmd(debugger, command, result, internal_dict):
    """Show the struct offsets LLDB is using for Enki decoding."""
    target = debugger.GetSelectedTarget()
    if not _is_valid(target):
        raise EnkiError("no selected target")
    layout = Layout(target)
    lines = [
        f"addr_size={layout.addr_size} size_t={layout.size_t_size} enki_value={layout.enki_value_size} mp_limb_t={layout.mp_limb_size} byte_order={layout.byte_order}",
    ]
    for desc in layout.all_descs():
        src = "DWARF" if desc.from_debug_info else "fallback"
        lines.append(f"{desc.name}: size={desc.size} source={src}")
        for name, (off, width) in sorted(desc.fields.items(), key=lambda kv: kv[1][0]):
            lines.append(f"  +{off:<3} {name:<14} width={width}")
    _append_lines(result, lines)


def enki_value_summary(valobj, internal_dict):
    """One-line LLDB type summary for enki_value."""
    try:
        v = valobj.GetValueAsUnsigned(0) & VALUE_MASK
        if not is_enki_ptr(v):
            return f"imm {_hex(v)} ({v})"
        target = valobj.GetTarget()
        if not _is_valid(target) or not _is_valid(target.GetProcess()):
            return f"ptr tagged={_hex(v)} ptr={_hex_addr(enki_to_ptr(v))}"
        return EnkiInspector(target).brief(v)
    except Exception as e:
        try:
            raw = valobj.GetValue()
        except Exception:
            raw = "?"
        return f"enki_value({raw}) decode-error: {e}"


_COMMANDS = [
    ("enki", "enki_cmd", "pretty-print an enki_value expression; options: -d/--depth, -n/--max-items, -b/--max-bytes, -l/--max-limbs"),
    ("enki-obj", "enki_obj_cmd", "pretty-print a raw enki heap object pointer"),
    ("enki-header", "enki_header_cmd", "print obj_header for a tagged enki_value"),
    ("enki-obj-header", "enki_obj_header_cmd", "print obj_header for a raw heap object pointer"),
    ("enki-tag", "enki_tag_cmd", "print PTR_TO_ENKI(pointer)"),
    ("enki-untag", "enki_untag_cmd", "print ENKI_TO_PTR(value)"),
    ("enki-nat", "enki_nat_cmd", "decode a BIG_NAT/NAT value"),
    ("enki-law", "enki_law_cmd", "decode a LAW value"),
    ("enki-layout", "enki_layout_cmd", "show decoded struct offsets"),
]


def _run_lldb_command(debugger, command: str):
    if lldb is None:
        return None
    res = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(command, res)
    return res


def __lldb_init_module(debugger, internal_dict):
    module = __name__
    for name, fn, help_text in _COMMANDS:
        del help_text
        _run_lldb_command(debugger, f"command script add -f {module}.{fn} {name}")

    # Put the summary in its own category so it is easy to disable:
    #   (lldb) type category disable enki
    _run_lldb_command(debugger, "type category define enki")
    _run_lldb_command(debugger, "type summary add -w enki -F %s.enki_value_summary enki_value" % module)
    _run_lldb_command(debugger, "type category enable enki")

    print("enki lldb utilities loaded: enki, enki-obj, enki-header, enki-tag, enki-untag, enki-nat, enki-law, enki-layout")
