# enki_lldb.py
#
# LLDB helpers for the er_val top-byte tagged representation from enki/run.h.
#
# Load in LLDB:
#   (lldb) command script import /absolute/path/to/lldb.py
#
# Then try:
#   (lldb) enki x_v
#   (lldb) enki -d 4 app_v
#   (lldb) enki-obj -t app some_er_app_ptr
#   (lldb) enki-header x_v
#   (lldb) enki-obj-header some_er_app_ptr
#   (lldb) enki-tag -t app some_raw_ptr
#   (lldb) enki-untag x_v
#   (lldb) enki-layout
#
# The module also registers type summaries for variables whose debug type is
# `er_val` or the older `enki_value` alias.

from __future__ import annotations

import shlex
import traceback
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

try:
    import lldb  # type: ignore
except Exception:  # lets `python -m py_compile lldb.py` work outside LLDB
    lldb = None  # type: ignore


VALUE_MASK = (1 << 64) - 1
ER_TAG_SHIFT = 56
ER_PTR_MASK = 0x00FFFFFFFFFFFFFF
ER_PTR_BIT = 1 << 63

ER_TAG_BAT = 0x82
ER_TAG_PIN = 0xC4
ER_TAG_LAW = 0xC8
ER_TAG_APP = 0xD0
ER_TAG_THK = 0xE0
ER_TAG_FWD = 0xF0
ER_TAG_TANK = 0xFE
ER_TAG_BAD = 0xFF

TAGS = {
    ER_TAG_BAT: "BAT",
    ER_TAG_PIN: "PIN",
    ER_TAG_LAW: "LAW",
    ER_TAG_APP: "APP",
    ER_TAG_THK: "THK",
    ER_TAG_FWD: "FWD",
    ER_TAG_TANK: "TANK",
    ER_TAG_BAD: "BAD",
}

TAG_NAMES = {
    "bat": ER_TAG_BAT,
    "big_nat": ER_TAG_BAT,
    "bignat": ER_TAG_BAT,
    "nat": ER_TAG_BAT,
    "pin": ER_TAG_PIN,
    "law": ER_TAG_LAW,
    "app": ER_TAG_APP,
    "thk": ER_TAG_THK,
    "thunk": ER_TAG_THK,
    "fwd": ER_TAG_FWD,
    "forward": ER_TAG_FWD,
    "tank": ER_TAG_TANK,
    "bad": ER_TAG_BAD,
}

HEADED_TAGS = {
    ER_TAG_BAT,
    ER_TAG_PIN,
    ER_TAG_LAW,
    ER_TAG_APP,
    ER_TAG_THK,
    ER_TAG_FWD,
}

KNOWN_TAGS = HEADED_TAGS | {ER_TAG_TANK, ER_TAG_BAD}

EXECF = {
    0: "ER_XDONE",
    1: "ER_XUNK_APP",
    2: "ER_CALL",
    3: "ER_XPRIM",
    4: "ER_HOLE",
    5: "ER_SUSP",
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


def er_get_tag(v: int) -> int:
    return ((v & VALUE_MASK) >> ER_TAG_SHIFT) & 0xFF


def tag_name(tag: int) -> str:
    return TAGS.get(tag, f"UNKNOWN(0x{tag:02x})")


def is_er_ptr(v: int) -> bool:
    return bool(v & ER_PTR_BIT)


def is_er_cat(v: int) -> bool:
    return not is_er_ptr(v)


def er_outa(v: int) -> int:
    return v & ER_PTR_MASK


def er_into(tag: int, ptr: int) -> int:
    return (((tag & 0xFF) << ER_TAG_SHIFT) | (ptr & ER_PTR_MASK)) & VALUE_MASK


def is_good_tag(tag: int) -> bool:
    return tag < ER_TAG_TANK


def is_whnf_tag(tag: int) -> bool:
    return tag <= ER_TAG_APP


# Backward-compatible names used by callers and older examples.
def is_enki_ptr(v: int) -> bool:
    return is_er_ptr(v)


def enki_to_ptr(v: int) -> int:
    return er_outa(v)


def ptr_to_enki(p: int, tag: int = ER_TAG_APP) -> int:
    return er_into(tag, p)


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


def _parse_tag(s: str) -> Optional[int]:
    cleaned = s.strip()
    if not cleaned:
        return None
    lowered = cleaned.lower()
    if lowered.startswith("er_tag_"):
        lowered = lowered[len("er_tag_") :]
    if lowered in TAG_NAMES:
        return TAG_NAMES[lowered]
    try:
        tag = int(cleaned, 0)
    except Exception:
        return None
    if 0 <= tag <= 0xFF:
        return tag
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
    """Target-dependent object layout for the run.h runtime."""

    def __init__(self, target):
        self.target = target
        self.addr_size = 8
        self.size_t_size = 8
        self.er_val_size = 8
        self.limb_size = 8
        self.execf_size = 4
        self.bcpc_size = 4
        self.byte_order = "little"

        if _is_valid(target):
            try:
                self.addr_size = int(target.GetAddressByteSize()) or 8
            except Exception:
                pass
            self.byte_order = self._target_byte_order(target)
            self.size_t_size = self._sizeof_first(["size_t"], self.addr_size)
            self.er_val_size = self._sizeof_first(["er_val", "enki_value", "uint64_t"], 8)
            self.limb_size = self._sizeof_first(["uint64_t"], 8)
            self.execf_size = self._sizeof_first(["er_execf"], 4)
            self.bcpc_size = self._sizeof_first(["er_bcpc", "uint32_t"], 4)

        ss = self.size_t_size
        ev = self.er_val_size
        hsz = ss

        self.head = self._struct_or_fallback(
            ["er_head"],
            StructDesc(
                "er_head",
                hsz,
                {
                    "siz_s": (0, ss),
                    "raw": (0, ss),
                },
                False,
            ),
        )

        h = self.head.size

        self.tank = self._struct_or_fallback(
            ["er_tank"],
            StructDesc(
                "er_tank",
                ev + self.addr_size,
                {
                    "val_v": (0, ev),
                    "msg_c": (_align_up(ev, self.addr_size), self.addr_size),
                },
                False,
            ),
        )

        self.bat = self._struct_or_fallback(
            ["er_bat"],
            StructDesc(
                "er_bat",
                h + ss,
                {
                    "hed": (0, h),
                    "lim_s": (h, ss),
                    "lim_q": (h + ss, self.limb_size),
                },
                False,
            ),
        )

        self.pin = self._struct_or_fallback(
            ["er_pin"],
            StructDesc(
                "er_pin",
                h + 32 + ev + ss,
                {
                    "hed": (0, h),
                    "hash_b": (h, 32),
                    "val_v": (h + 32, ev),
                    "sub_s": (h + 32 + ev, ss),
                    "sub_v": (h + 32 + ev + ss, ev),
                },
                False,
            ),
        )

        self.law_label = self._struct_or_fallback(
            ["er_law_label"],
            StructDesc(
                "er_law_label",
                _align_up(self.bcpc_size, ss) + ss,
                {
                    "pc": (0, self.bcpc_size),
                    "op_s": (_align_up(self.bcpc_size, ss), ss),
                },
                False,
            ),
        )

        law_ari_off = h + ev * 2
        law_let_off = law_ari_off + 4
        law_bc_s_off = _align_up(law_let_off + 4, ss)
        law_op_s_off = law_bc_s_off + ss
        law_bc_v_off = law_op_s_off + ss
        self.law = self._struct_or_fallback(
            ["er_law"],
            StructDesc(
                "er_law",
                law_bc_v_off,
                {
                    "h": (0, h),
                    "name_v": (h, ev),
                    "body_v": (h + ev, ev),
                    "ari_d": (law_ari_off, 4),
                    "let_d": (law_let_off, 4),
                    "bc_s": (law_bc_s_off, ss),
                    "op_s": (law_op_s_off, ss),
                    "bc_v": (law_bc_v_off, self.law_label.size),
                },
                False,
            ),
        )

        self.app = self._struct_or_fallback(
            ["er_app"],
            StructDesc(
                "er_app",
                h + ev + ss,
                {
                    "h": (0, h),
                    "fn_v": (h, ev),
                    "arg_s": (h + ev, ss),
                    "arg_v": (h + ev + ss, ev),
                },
                False,
            ),
        )

        thk_fun_off = h
        thk_arg_s_off = _align_up(thk_fun_off + self.execf_size, ss)
        thk_arg_v_off = thk_arg_s_off + ss
        self.thk = self._struct_or_fallback(
            ["er_thk"],
            StructDesc(
                "er_thk",
                thk_arg_v_off,
                {
                    "hed": (0, h),
                    "fun": (thk_fun_off, self.execf_size),
                    "arg_s": (thk_arg_s_off, ss),
                    "arg_v": (thk_arg_v_off, ev),
                },
                False,
            ),
        )

    @property
    def enki_value_size(self) -> int:
        return self.er_val_size

    @property
    def mp_limb_size(self) -> int:
        return self.limb_size

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
        return [self.head, self.tank, self.bat, self.pin, self.law_label, self.law, self.app, self.thk]


def _align_up(n: int, align: int) -> int:
    if align <= 1:
        return n
    return (n + align - 1) & ~(align - 1)


@dataclass
class Head:
    raw: int

    @property
    def size(self) -> int:
        return self.raw & ~0x3

    @property
    def fwd(self) -> int:
        return self.raw & 0x1

    @property
    def nf(self) -> int:
        return (self.raw >> 1) & 0x1

    def state_name(self, tag: int) -> str:
        if self.fwd:
            return "FWD"
        if tag == ER_TAG_THK:
            return "THUNK"
        if tag in (ER_TAG_BAT, ER_TAG_PIN, ER_TAG_LAW, ER_TAG_APP):
            return "NF" if self.nf else "WHNF"
        return "HEAD"


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

    def read_er_val(self, addr: int) -> int:
        return self.read_uint(addr, self.layout.er_val_size) & VALUE_MASK

    def read_ptr(self, addr: int) -> int:
        return self.read_uint(addr, self.layout.addr_size)

    def read_head(self, ptr: int) -> Head:
        d = self.layout.head
        raw = self.read_uint(ptr + d.off("siz_s", 0), d.width("siz_s", self.layout.size_t_size))
        return Head(raw=raw)

    def read_cstring(self, addr: int, max_bytes: int = 256) -> str:
        if addr == 0:
            return "<null>"
        chunks: List[int] = []
        for off in range(max_bytes):
            b = self.read_uint(addr + off, 1)
            if b == 0:
                break
            chunks.append(b)
        else:
            chunks.extend(ord(c) for c in "...")
        return bytes(chunks).decode("utf-8", errors="replace")


class EnkiInspector:
    def __init__(self, target):
        self.mem = Memory(target)
        self.layout = self.mem.layout

    def brief(self, v: int) -> str:
        v &= VALUE_MASK
        if is_er_cat(v):
            return f"cat {_hex(v)} ({v})"

        tag = er_get_tag(v)
        ptr = er_outa(v)
        name = tag_name(tag)
        if tag == ER_TAG_BAD:
            return f"{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)}"
        if tag == ER_TAG_TANK:
            return f"{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)}"
        if tag not in HEADED_TAGS:
            return f"{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)}"

        try:
            h = self.mem.read_head(ptr)
            return (
                f"{name}/{h.state_name(tag)} "
                f"tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} size={h.size}"
            )
        except Exception as e:
            return f"{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} unreadable: {e}"

    def head_lines(self, ptr: int, *, tag: Optional[int] = None) -> List[str]:
        h = self.mem.read_head(ptr)
        lines = [
            f"ptr:    {_hex_addr(ptr, self.layout.addr_size)}",
            f"head:   {_hex(h.raw)}",
            f"size:   {h.size}",
            f"fwd_f:  {h.fwd}",
            f"nf_f:   {h.nf}",
        ]
        if tag is not None:
            lines.insert(0, f"tagged: {_hex(er_into(tag, ptr))}")
            lines.insert(2, f"tag:    0x{tag:02x} ({tag_name(tag)})")
            lines.append(f"state:  {h.state_name(tag)}")
        if h.fwd:
            slot = ptr + self.layout.head.size
            try:
                target = self.mem.read_er_val(slot)
                lines.append(f"fwd_to: {self.brief(target)}")
            except Exception as e:
                lines.append(f"fwd_to: unreadable at {_hex_addr(slot, self.layout.addr_size)}: {e}")
        return lines

    def header_lines(self, v_or_ptr: int, *, raw_ptr: bool = False, tag: Optional[int] = None) -> List[str]:
        if raw_ptr:
            ptr = er_outa(v_or_ptr) if is_er_ptr(v_or_ptr) else v_or_ptr
            return self.head_lines(ptr, tag=tag)

        if is_er_cat(v_or_ptr):
            return [f"not a tagged er_val pointer: {_hex(v_or_ptr)} ({v_or_ptr})"]

        tagged = v_or_ptr & VALUE_MASK
        tag = er_get_tag(tagged)
        ptr = er_outa(tagged)
        lines = [
            f"tagged: {_hex(tagged)}",
            f"tag:    0x{tag:02x} ({tag_name(tag)})",
            f"ptr:    {_hex_addr(ptr, self.layout.addr_size)}",
        ]
        if tag == ER_TAG_TANK:
            lines.append("head:   <none; er_tank is not headed>")
            return lines
        if tag == ER_TAG_BAD:
            lines.append("head:   <none; er_bad sentinel>")
            return lines
        lines.extend(self.head_lines(ptr, tag=tag)[3:])
        return lines

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

        if is_er_cat(v):
            return [f"{ind}cat {_hex(v)} ({v})"]

        tag = er_get_tag(v)
        ptr = er_outa(v)
        name = tag_name(tag)

        if tag == ER_TAG_BAD:
            return [f"{ind}{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)}"]

        if tag == ER_TAG_TANK:
            head = f"{ind}{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)}"
            if depth <= 0:
                return [head]
            if ptr in _seen:
                return [head + " (seen)"]
            _seen.add(ptr)
            try:
                lines = [head]
                lines.extend(self._describe_tank(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            except Exception as e:
                return [head, f"{ind}  error while decoding payload: {e}"]

        if tag not in HEADED_TAGS:
            return [f"{ind}{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)}"]

        try:
            h = self.mem.read_head(ptr)
        except Exception as e:
            return [f"{ind}{name} tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} unreadable: {e}"]

        head = (
            f"{ind}{name}/{h.state_name(tag)} "
            f"tagged={_hex(v)} ptr={_hex_addr(ptr, self.layout.addr_size)} size={h.size}"
        )
        if depth <= 0:
            return [head]
        if ptr in _seen:
            return [head + " (seen)"]
        _seen.add(ptr)

        try:
            if h.fwd or tag == ER_TAG_FWD:
                lines = [head]
                lines.extend(self._describe_fwd(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if tag == ER_TAG_BAT:
                lines = [head]
                lines.extend(self._describe_bat(ptr, max_nat_limbs, _indent + 1))
                return lines
            if tag == ER_TAG_PIN:
                lines = [head]
                lines.extend(self._describe_pin(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if tag == ER_TAG_LAW:
                lines = [head]
                lines.extend(self._describe_law(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if tag == ER_TAG_APP:
                lines = [head]
                lines.extend(self._describe_app(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            if tag == ER_TAG_THK:
                lines = [head]
                lines.extend(self._describe_thk(ptr, depth, max_items, max_bytes, max_nat_limbs, _indent + 1, _seen))
                return lines
            return [head, f"{ind}  unknown headed tag; raw payload starts at +{self.layout.head.size}"]
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
        if depth > 1 and is_er_ptr(value) and er_get_tag(value) in KNOWN_TAGS:
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

    def _describe_tank(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.tank
        ind = "  " * indent
        val = self.mem.read_er_val(ptr + d.off("val_v"))
        msg_ptr = self.mem.read_ptr(ptr + d.off("msg_c"))
        msg = self.mem.read_cstring(msg_ptr, max_bytes)
        lines = [f"{ind}msg_c: {msg!r} @ {_hex_addr(msg_ptr, self.layout.addr_size)}"]
        lines.extend(self._describe_child_value("val_v", val, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        return lines

    def _describe_pin(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.pin
        ind = "  " * indent
        hash_b = self.mem.read_bytes(ptr + d.off("hash_b"), min(32, d.width("hash_b", 32)))[:32]
        val = self.mem.read_er_val(ptr + d.off("val_v"))
        sub_s = self.mem.read_uint(ptr + d.off("sub_s"), d.width("sub_s", self.layout.size_t_size))
        sub_base = ptr + d.off("sub_v")

        lines = [f"{ind}hash_b: {hash_b.hex()}"]
        lines.extend(self._describe_child_value("val_v", val, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        lines.append(f"{ind}sub_s: {sub_s}")
        for i in range(min(sub_s, max_items)):
            sv = self.mem.read_er_val(sub_base + i * self.layout.er_val_size)
            lines.extend(self._describe_child_value(f"sub_v[{i}]", sv, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if sub_s > max_items:
            lines.append(f"{ind}  ... {sub_s - max_items} more sub value(s) not shown")
        return lines

    def _describe_law(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.law
        ld = self.layout.law_label
        ind = "  " * indent
        name = self.mem.read_er_val(ptr + d.off("name_v"))
        body = self.mem.read_er_val(ptr + d.off("body_v"))
        ari = self.mem.read_uint(ptr + d.off("ari_d"), d.width("ari_d", 4))
        let = self.mem.read_uint(ptr + d.off("let_d"), d.width("let_d", 4))
        bc_s = self.mem.read_uint(ptr + d.off("bc_s"), d.width("bc_s", self.layout.size_t_size))
        op_s = self.mem.read_uint(ptr + d.off("op_s"), d.width("op_s", self.layout.size_t_size))
        label_base = ptr + d.off("bc_v")

        lines = [
            f"{ind}ari_d: {ari}",
            f"{ind}let_d: {let}",
            f"{ind}bc_s:  {bc_s}",
            f"{ind}op_s:  {op_s}",
        ]
        lines.extend(self._describe_child_value("name_v", name, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        lines.extend(self._describe_child_value("body_v", body, depth, max_items, max_bytes, max_nat_limbs, indent, seen))

        lines.append(f"{ind}bc_v @ {_hex_addr(label_base, self.layout.addr_size)}")
        for i in range(min(bc_s, max_items)):
            base = label_base + i * ld.size
            pc = self.mem.read_uint(base + ld.off("pc"), ld.width("pc", self.layout.bcpc_size))
            ops = self.mem.read_uint(base + ld.off("op_s"), ld.width("op_s", self.layout.size_t_size))
            lines.append(f"{ind}  bc_v[{i}]: pc={pc} op_s={ops}")
        if bc_s > max_items:
            lines.append(f"{ind}  ... {bc_s - max_items} more label(s) not shown")
        return lines

    def _describe_bat(self, ptr: int, max_nat_limbs: int, indent: int) -> List[str]:
        d = self.layout.bat
        ind = "  " * indent
        lim_s = self.mem.read_uint(ptr + d.off("lim_s"), d.width("lim_s", self.layout.size_t_size))
        limb_base = ptr + d.off("lim_q")
        limb_size = self.layout.limb_size
        limb_bits = limb_size * 8
        n_show = min(lim_s, max_nat_limbs)
        limbs = [self.mem.read_uint(limb_base + i * limb_size, limb_size) for i in range(n_show)]

        lines = [f"{ind}lim_s: {lim_s}", f"{ind}lim_q @ {_hex_addr(limb_base, self.layout.addr_size)}"]
        if lim_s <= max_nat_limbs:
            value = 0
            for i, limb in enumerate(limbs):
                value |= limb << (i * limb_bits)
            lines.append(f"{ind}value_dec: {value}")
            lines.append(f"{ind}value_hex: {hex(value)}")
        else:
            lines.append(f"{ind}value_dec: <not computed; {lim_s} limbs exceeds max {max_nat_limbs}>")
        if limbs:
            rendered = ", ".join(f"{i}:{_limb_hex(x, limb_size)}" for i, x in enumerate(limbs[: min(len(limbs), 16)]))
            lines.append(f"{ind}limbs[least-significant first]: {rendered}")
            if n_show > 16:
                lines.append(f"{ind}  ... {n_show - 16} more displayed limb(s) omitted from this line")
        else:
            lines.append(f"{ind}limbs: []")
        if lim_s > max_nat_limbs:
            lines.append(f"{ind}  ... {lim_s - max_nat_limbs} limb(s) not read")
        return lines

    def _describe_app(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.app
        ind = "  " * indent
        fn = self.mem.read_er_val(ptr + d.off("fn_v"))
        arg_s = self.mem.read_uint(ptr + d.off("arg_s"), d.width("arg_s", self.layout.size_t_size))
        args_base = ptr + d.off("arg_v")
        lines = []
        lines.extend(self._describe_child_value("fn_v", fn, depth, max_items, max_bytes, max_nat_limbs, indent, seen))
        lines.append(f"{ind}arg_s: {arg_s}")
        for i in range(min(arg_s, max_items)):
            av = self.mem.read_er_val(args_base + i * self.layout.er_val_size)
            lines.extend(self._describe_child_value(f"arg_v[{i}]", av, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if arg_s > max_items:
            lines.append(f"{ind}  ... {arg_s - max_items} more arg(s) not shown")
        return lines

    def _describe_thk(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        d = self.layout.thk
        ind = "  " * indent
        fun = self.mem.read_uint(ptr + d.off("fun"), d.width("fun", self.layout.execf_size))
        arg_s = self.mem.read_uint(ptr + d.off("arg_s"), d.width("arg_s", self.layout.size_t_size))
        args_base = ptr + d.off("arg_v")
        lines = [f"{ind}fun: {fun} ({EXECF.get(fun, 'UNKNOWN')})", f"{ind}arg_s: {arg_s}"]
        for i in range(min(arg_s, max_items)):
            av = self.mem.read_er_val(args_base + i * self.layout.er_val_size)
            lines.extend(self._describe_child_value(f"arg_v[{i}]", av, depth, max_items, max_bytes, max_nat_limbs, indent + 1, seen))
        if arg_s > max_items:
            lines.append(f"{ind}  ... {arg_s - max_items} more arg(s) not shown")
        return lines

    def _describe_fwd(self, ptr: int, depth: int, max_items: int, max_bytes: int, max_nat_limbs: int, indent: int, seen: set) -> List[str]:
        ind = "  " * indent
        payload = ptr + self.layout.head.size
        try:
            target = self.mem.read_er_val(payload)
        except Exception as e:
            return [f"{ind}forwarding slot unreadable at {_hex_addr(payload, self.layout.addr_size)}: {e}"]
        lines = [f"{ind}forwarding slot @ {_hex_addr(payload, self.layout.addr_size)}: {self.brief(target)}"]
        if depth > 1 and is_er_ptr(target) and er_get_tag(target) in KNOWN_TAGS:
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


def _parse_common(command: str, *, allow_tag: bool = False) -> Tuple[Dict[str, Optional[int]], str]:
    tokens = shlex.split(command)
    opts: Dict[str, Optional[int]] = {
        "depth": 2,
        "max_items": 16,
        "max_bytes": 64,
        "max_nat_limbs": 256,
        "tag": None,
    }
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
        elif allow_tag and t in ("-t", "--tag"):
            i += 1
            if i >= len(tokens):
                raise EnkiError("missing value after --tag")
            tag = _parse_tag(tokens[i])
            if tag is None:
                raise EnkiError(f"unknown er_val tag {tokens[i]!r}")
            opts["tag"] = tag
        elif allow_tag and t.startswith("--tag="):
            tag = _parse_tag(t.split("=", 1)[1])
            if tag is None:
                raise EnkiError(f"unknown er_val tag {t!r}")
            opts["tag"] = tag
        elif t == "--":
            expr_tokens = tokens[i + 1 :]
            break
        else:
            if allow_tag and opts["tag"] is None and i + 1 < len(tokens):
                tag = _parse_tag(t)
                if tag is not None:
                    opts["tag"] = tag
                    expr_tokens = tokens[i + 1 :]
                    break
            expr_tokens = tokens[i:]
            break
        i += 1

    expr = " ".join(expr_tokens).strip()
    if not expr:
        raise EnkiError("missing expression")
    return opts, expr


def _int_opt(opts: Dict[str, Optional[int]], key: str) -> int:
    value = opts[key]
    if value is None:
        raise EnkiError(f"missing option {key}")
    return value


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
    """Pretty-print an er_val expression."""
    opts, expr = _parse_common(command)
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    _append_lines(
        result,
        ins.describe(
            v,
            depth=_int_opt(opts, "depth"),
            max_items=_int_opt(opts, "max_items"),
            max_bytes=_int_opt(opts, "max_bytes"),
            max_nat_limbs=_int_opt(opts, "max_nat_limbs"),
        ),
    )


@_guarded
def enki_obj_cmd(debugger, command, result, internal_dict):
    """Pretty-print a raw er_* heap object pointer with an explicit top-byte tag."""
    opts, expr = _parse_common(command, allow_tag=True)
    raw = eval_u64(debugger, expr)
    if is_er_ptr(raw):
        v = raw
    else:
        tag = opts["tag"]
        if tag is None:
            raise EnkiError("raw object pointers do not encode kind; pass -t/--tag (app, law, pin, bat, thk, tank)")
        v = er_into(tag, raw)
    ins = _get_inspector(debugger)
    _append_lines(
        result,
        ins.describe(
            v,
            depth=_int_opt(opts, "depth"),
            max_items=_int_opt(opts, "max_items"),
            max_bytes=_int_opt(opts, "max_bytes"),
            max_nat_limbs=_int_opt(opts, "max_nat_limbs"),
        ),
    )


@_guarded
def enki_header_cmd(debugger, command, result, internal_dict):
    """Print the er_val tag and er_head for a tagged value."""
    opts, expr = _parse_common(command)
    del opts
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    _append_lines(result, ins.header_lines(v, raw_ptr=False))


@_guarded
def enki_obj_header_cmd(debugger, command, result, internal_dict):
    """Print the er_head for a raw heap object pointer."""
    opts, expr = _parse_common(command, allow_tag=True)
    ptr = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    _append_lines(result, ins.header_lines(ptr, raw_ptr=True, tag=opts["tag"]))


@_guarded
def enki_tag_cmd(debugger, command, result, internal_dict):
    """Apply er_into(tag, pointer) to a pointer expression/literal."""
    opts, expr = _parse_common(command, allow_tag=True)
    tag = opts["tag"]
    if tag is None:
        raise EnkiError("missing tag; use -t/--tag (app, law, pin, bat, thk, tank) or pass the tag before the expression")
    p = eval_u64(debugger, expr)
    tagged = er_into(tag, p)
    lines = [
        f"tag:    0x{tag:02x} ({tag_name(tag)})",
        f"ptr:    {_hex_addr(p & ER_PTR_MASK)}",
        f"tagged: {_hex(tagged)}",
        f"lldb expr: (er_val)(((uint64_t)0x{tag:02x} << 56) | ((uint64_t)(uintptr_t){expr} & UINT64_C(0x00ffffffffffffff)))",
    ]
    _append_lines(result, lines)


@_guarded
def enki_untag_cmd(debugger, command, result, internal_dict):
    """Apply er_get_tag and er_outa to an er_val expression/literal."""
    opts, expr = _parse_common(command)
    del opts
    v = eval_u64(debugger, expr)
    tag = er_get_tag(v)
    ptr = er_outa(v)
    lines = [
        f"value:  {_hex(v)}",
        f"tag:    0x{tag:02x} ({tag_name(tag)})",
        f"is_ptr: {1 if is_er_ptr(v) else 0}",
        f"is_cat: {1 if is_er_cat(v) else 0}",
        f"ptr:    {_hex_addr(ptr)}",
        f"lldb tag expr: (uint8_t)((uint64_t){expr} >> 56)",
        f"lldb ptr expr: (void*)((uint64_t){expr} & UINT64_C(0x00ffffffffffffff))",
    ]
    _append_lines(result, lines)


@_guarded
def enki_nat_cmd(debugger, command, result, internal_dict):
    """Decode a cat immediate or BAT value as decimal, hex, and limbs."""
    opts, expr = _parse_common(command)
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    if is_er_cat(v):
        _append_lines(result, [f"cat {_hex(v)} ({v})"])
        return
    tag = er_get_tag(v)
    if tag != ER_TAG_BAT:
        _append_lines(result, [f"warning: tag is {tag_name(tag)}, not BAT"] + ins.describe(v, depth=0))
        return
    lines = ins.describe(v, depth=1, max_nat_limbs=_int_opt(opts, "max_nat_limbs"))
    _append_lines(result, lines)


@_guarded
def enki_law_cmd(debugger, command, result, internal_dict):
    """Decode an er_law, including bytecode label metadata."""
    opts, expr = _parse_common(command)
    v = eval_u64(debugger, expr)
    ins = _get_inspector(debugger)
    if is_er_cat(v):
        _append_lines(result, [f"not a LAW heap pointer; cat {_hex(v)} ({v})"])
        return
    tag = er_get_tag(v)
    if tag != ER_TAG_LAW:
        _append_lines(result, [f"warning: tag is {tag_name(tag)}, not LAW"] + ins.describe(v, depth=0))
        return
    _append_lines(
        result,
        ins.describe(
            v,
            depth=_int_opt(opts, "depth"),
            max_items=_int_opt(opts, "max_items"),
            max_bytes=_int_opt(opts, "max_bytes"),
            max_nat_limbs=_int_opt(opts, "max_nat_limbs"),
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
        f"addr_size={layout.addr_size} size_t={layout.size_t_size} er_val={layout.er_val_size} limb={layout.limb_size} byte_order={layout.byte_order}",
        "tags: " + ", ".join(f"{name}=0x{tag:02x}" for tag, name in sorted(TAGS.items())),
        f"ptr_mask=0x{ER_PTR_MASK:016x} tag_shift={ER_TAG_SHIFT}",
    ]
    for desc in layout.all_descs():
        src = "DWARF" if desc.from_debug_info else "fallback"
        lines.append(f"{desc.name}: size={desc.size} source={src}")
        for name, (off, width) in sorted(desc.fields.items(), key=lambda kv: kv[1][0]):
            lines.append(f"  +{off:<3} {name:<10} width={width}")
    _append_lines(result, lines)


def enki_value_summary(valobj, internal_dict):
    """One-line LLDB type summary for er_val/enki_value."""
    try:
        v = valobj.GetValueAsUnsigned(0) & VALUE_MASK
        if is_er_cat(v):
            return f"cat {_hex(v)} ({v})"
        target = valobj.GetTarget()
        if not _is_valid(target) or not _is_valid(target.GetProcess()):
            tag = er_get_tag(v)
            return f"{tag_name(tag)} tagged={_hex(v)} ptr={_hex_addr(er_outa(v))}"
        return EnkiInspector(target).brief(v)
    except Exception as e:
        try:
            raw = valobj.GetValue()
        except Exception:
            raw = "?"
        return f"er_val({raw}) decode-error: {e}"


_COMMANDS = [
    ("enki", "enki_cmd", "pretty-print an er_val expression; options: -d/--depth, -n/--max-items, -b/--max-bytes, -l/--max-limbs"),
    ("enki-obj", "enki_obj_cmd", "pretty-print a raw er_* heap object pointer; requires -t/--tag unless the expression is already tagged"),
    ("enki-header", "enki_header_cmd", "print er_val tag and er_head for a tagged er_val"),
    ("enki-obj-header", "enki_obj_header_cmd", "print er_head for a raw heap object pointer; optional -t/--tag annotates kind"),
    ("enki-tag", "enki_tag_cmd", "print er_into(tag, pointer); requires -t/--tag"),
    ("enki-untag", "enki_untag_cmd", "print er_get_tag(value) and er_outa(value)"),
    ("enki-nat", "enki_nat_cmd", "decode a cat immediate or BAT value"),
    ("enki-law", "enki_law_cmd", "decode a LAW value"),
    ("enki-layout", "enki_layout_cmd", "show decoded struct offsets and tag constants"),
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

    # Put the summaries in their own category so they are easy to disable:
    #   (lldb) type category disable enki
    _run_lldb_command(debugger, "type category define enki")
    _run_lldb_command(debugger, "type summary add -w enki -F %s.enki_value_summary er_val" % module)
    _run_lldb_command(debugger, "type summary add -w enki -F %s.enki_value_summary enki_value" % module)
    _run_lldb_command(debugger, "type category enable enki")

    print("enki lldb utilities loaded: enki, enki-obj, enki-header, enki-tag, enki-untag, enki-nat, enki-law, enki-layout")
