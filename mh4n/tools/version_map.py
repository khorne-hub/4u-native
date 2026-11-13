#!/usr/bin/env python3
"""Map code and data addresses between two versions of the game.

The project's tables (config/functions.tsv, extra_entries.txt, hooks.txt, tunables.txt) and the
port's own addresses (config/port_addresses.txt) hold USA v1.3.0 addresses. Another version built
from the same source has its functions a little further along .text where code before them
changed: EUR v1.1.0 only there (its .rodata/.data/.bss layout is the same), USA v0.1.0 (the base
game, which Heavenfall's web installer builds on) in its data too, where an object was added or
grew. This tool finds, from both dumps, where each function went and writes the result as a short
list of shift intervals: USA addresses in [start, end) are at address + shift in the other
version. The list holds addresses only (no game data), so it is committed (config/shifts/) and
build_game.py applies it with only the other version's dump.

How a function is matched: the functions whose first 16 bytes occur exactly once in the other
code are anchors; the longest run of anchors in increasing order gives the candidate shifts
around each function, and a function is mapped when its bytes equal the other code's at one
of them, branch offsets that leave the function and code and data pointers set aside. Those
set-aside branches and pointers are then checked: each must land on the mapped address of its
target.

A version whose .rodata, .data or .bss differ gets a `data` section as well: the data addresses
that the matched functions' literal pools hold, in both versions, give each such address a
shift; neighbours with the same shift make an interval, and where two intervals with different
shifts meet in .rodata or .data the bytes of both dumps place the boundary. An address between
two such intervals in .bss (no bytes to compare) has no translation. A function whose data
pointers disagree with the intervals is dropped, as one whose branches disagree is.

usage: version_map.py build --src-exheader A/exheader.bin --src-code A/code.bin \\
           --dst-exheader B/exheader.bin --dst-code B/code.bin \\
           [--functions config/functions.tsv] [--name "EUR v1.1.0"] -o config/shifts/eur_v110.txt
       version_map.py translate config/shifts/eur_v110.txt 0x002951fc ...
"""
from __future__ import annotations

import argparse
import bisect
import collections
import hashlib
import os
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_function_table as bft  # noqa: E402
import codebin2elf  # noqa: E402
import exheader as exh  # noqa: E402

ANCHOR_BYTES = 16
MAX_POINTER_DELTA = 0x10000   # a code pointer that moved further than this isn't the same pointer
MAX_DATA_DELTA = 0x10000      # nor a data pointer


@dataclass(frozen=True)
class Image:
    base: int
    code: bytes
    text_end: int   # absolute
    # [start, end) of the data the code points to (.rodata to the end of .bss): a pointer there may
    # differ between two versions whose data moved. (0, 0): data pointers must be equal, as before.
    data: tuple[int, int] = (0, 0)
    # [start, end) of that data that has bytes in `code` (.rodata and .data, not .bss)
    initialized: tuple[int, int] = (0, 0)
    # the data's segments, (start, end, end of their bytes): .rodata, and .data with .bss after it
    segments: tuple[tuple[int, int, int], ...] = ()

    def in_text(self, addr: int) -> bool:
        return self.base <= (addr & ~1) < self.text_end

    def in_data(self, addr: int) -> bool:
        return self.data[0] <= addr < self.data[1]

    def word(self, addr: int) -> int | None:
        off = addr - self.base
        if addr & 3 or off < 0 or off + 4 > len(self.code):
            return None
        return struct.unpack_from("<I", self.code, off)[0]


@dataclass(frozen=True)
class Function:
    addr: int
    size: int
    thumb: bool


class Shifts:
    """Sorted, non-overlapping intervals [start, end) of source .text with their shift, and, for a
    version whose data moved too, the same for its data (.rodata to the end of .bss)."""

    def __init__(self, intervals: list[tuple[int, int, int]], text: tuple[int, int] | None = None,
                 header: list[str] | None = None, data_intervals: list[tuple[int, int, int]] | None = None,
                 data: tuple[int, int] | None = None):
        self.intervals = sorted(intervals)
        self.starts = [s for s, _, _ in self.intervals]
        # source .text [base, end): addresses outside it (and outside `data`) are the same in both
        self.text = text
        self.header = header or []
        self.data = data   # source [.rodata, end of .bss) when the data moved, else None
        self.data_intervals = sorted(data_intervals or [])
        self.data_starts = [s for s, _, _ in self.data_intervals]

    def translate(self, addr: int) -> int | None:
        """The other version's address, or None for a .text address (or one in data that moved) in
        no interval. A code address keeps its Thumb bit; a data address is a byte address."""
        if self.data and self.data[0] <= addr < self.data[1]:
            i = bisect.bisect_right(self.data_starts, addr) - 1
            if i >= 0 and addr < self.data_intervals[i][1]:
                return addr + self.data_intervals[i][2]
            return None
        thumb, a = addr & 1, addr & ~1
        if self.text and not self.text[0] <= a < self.text[1]:
            return addr
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and a < self.intervals[i][1]:
            return (a + self.intervals[i][2]) | thumb
        return None

    def translate_keys(self, table: dict[int, object]) -> tuple[dict[int, object], list[int]]:
        """A table keyed by address, re-keyed; and the keys that have no mapping."""
        out, missing = {}, []
        for a, v in table.items():
            t = self.translate(a)
            if t is None:
                missing.append(a)
            else:
                out[t] = v
        return out, sorted(missing)

    def dumps(self) -> str:
        lines = [f"# {h}" for h in self.header]
        if self.text:
            lines.append(f"text 0x{self.text[0]:08x} 0x{self.text[1]:08x}")
        lines += [f"0x{s:08x} 0x{e:08x} {d:+#x}" for s, e, d in self.intervals]
        if self.data:
            lines.append(f"data 0x{self.data[0]:08x} 0x{self.data[1]:08x}")
            lines += [f"0x{s:08x} 0x{e:08x} {d:+#x}" for s, e, d in self.data_intervals]
        return "\n".join(lines) + "\n"

    @classmethod
    def loads(cls, text: str) -> "Shifts":
        intervals, rng, header = [], None, []
        data_intervals, data = [], None
        current = intervals
        for line in text.splitlines():
            if line.startswith("#"):
                header.append(line[1:].strip())
                continue
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "text":
                rng = (int(parts[1], 16), int(parts[2], 16))
                current = intervals
            elif parts[0] == "data":
                data = (int(parts[1], 16), int(parts[2], 16))
                current = data_intervals
            else:
                current.append((int(parts[0], 16), int(parts[1], 16), int(parts[2], 16)))
        return cls(intervals, rng, header, data_intervals, data)

    @classmethod
    def load(cls, path: str) -> "Shifts":
        with open(path, encoding="utf-8") as f:
            return cls.loads(f.read())


# ── comparing code ─────────────────────────────────────────────────────────

def _arm_branch(w: int) -> bool:
    """B, BL or BLX (immediate)."""
    return (w & 0x0E000000) == 0x0A000000


def _arm_target(w: int, pc: int) -> int:
    imm = w & 0xFFFFFF
    imm -= (imm & 0x800000) << 1
    target = pc + 8 + (imm << 2)
    if (w >> 28) == 0xF:   # BLX: to Thumb, H bit gives the halfword
        target |= ((w >> 24) & 1) << 1 | 1
    return target & 0xFFFFFFFF


def _thumb_bl(h1: int, h2: int) -> bool:
    return (h1 & 0xF800) == 0xF000 and (h2 & 0xE800) == 0xE800


def _thumb_bl_target(h1: int, h2: int, pc: int) -> int:
    off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
    off -= (off & 0x400000) << 1
    target = pc + 4 + off
    return (target | 1) if h2 & 0x1000 else (target & ~3)   # BL stays Thumb, BLX goes to ARM


def _thumb_b_target(h: int, pc: int) -> int | None:
    if (h & 0xF800) == 0xE000:            # B (unconditional)
        off = (h & 0x7FF) << 1
        off -= (off & 0x800) << 1
    elif (h & 0xF000) == 0xD000 and (h & 0x0F00) < 0x0E00:   # B<cond>
        off = (h & 0xFF) << 1
        off -= (off & 0x100) << 1
    else:
        return None
    return pc + 4 + off


def _pointer_pair(src: Image, dst: Image, wu: int, we: int) -> bool:
    if src.in_data(wu) and dst.in_data(we):   # (only when the images say where their data is)
        return (wu & 3) == (we & 3) and abs(we - wu) < MAX_DATA_DELTA
    return (src.in_text(wu) and dst.in_text(we) and (wu & 1) == (we & 1)
            and abs(we - wu) < MAX_POINTER_DELTA)


def compare(src: Image, dst: Image, sa: int, da: int, size: int, thumb: bool) -> list[tuple[int, int]] | None:
    """Whether `size` bytes at sa (source) and da (destination) are the same code once
    branches out of the range and code (and data) pointers are set aside. Returns those set-aside
    (source target, destination target) pairs, or None if the code differs. A branch
    inside the range must keep its offset within it."""
    so, do = sa - src.base, da - dst.base
    if so < 0 or do < 0 or so + size > len(src.code) or do + size > len(dst.code):
        return None
    s, d = src.code, dst.code
    pairs: list[tuple[int, int]] = []

    def branch(tu: int, te: int) -> bool:
        if sa <= (tu & ~1) < sa + size:
            return (te & ~1) - da == (tu & ~1) - sa
        pairs.append((tu, te))
        return True

    k = 0
    if not thumb:
        while k + 4 <= size:
            wu = struct.unpack_from("<I", s, so + k)[0]
            we = struct.unpack_from("<I", d, do + k)[0]
            if _arm_branch(wu) and _arm_branch(we) and (wu >> 24) == (we >> 24):
                if not branch(_arm_target(wu, sa + k), _arm_target(we, da + k)):
                    return None
            elif wu != we:
                if not _pointer_pair(src, dst, wu, we):
                    return None
                pairs.append((wu, we))
            k += 4
        return pairs if s[so + k:so + size] == d[do + k:do + size] else None
    while k + 2 <= size:
        hu = struct.unpack_from("<H", s, so + k)[0]
        he = struct.unpack_from("<H", d, do + k)[0]
        if k + 4 <= size:
            hu2 = struct.unpack_from("<H", s, so + k + 2)[0]
            he2 = struct.unpack_from("<H", d, do + k + 2)[0]
            if _thumb_bl(hu, hu2) and _thumb_bl(he, he2) and (hu2 & 0x1000) == (he2 & 0x1000):
                if not branch(_thumb_bl_target(hu, hu2, sa + k), _thumb_bl_target(he, he2, da + k)):
                    return None
                k += 4
                continue
        if hu != he:
            tu, te = _thumb_b_target(hu, sa + k), _thumb_b_target(he, da + k)
            if tu is not None and te is not None and (hu & 0xFF00 if hu & 0xF000 == 0xD000 else 0xE000) == \
                    (he & 0xFF00 if he & 0xF000 == 0xD000 else 0xE000):
                if not branch(tu | 1, te | 1):
                    return None
                k += 2
                continue
            if k % 4 == 0 and k + 4 <= size:   # a literal word holding a code pointer
                wu = struct.unpack_from("<I", s, so + k)[0]
                we = struct.unpack_from("<I", d, do + k)[0]
                if _pointer_pair(src, dst, wu, we):
                    pairs.append((wu, we))
                    k += 4
                    continue
            return None
        k += 2
    return pairs if s[so + k:so + size] == d[do + k:do + size] else None


# ── building the map ───────────────────────────────────────────────────────

def _anchors(src: Image, dst: Image, funcs: list[Function]) -> list[tuple[int, int]]:
    """(source, destination) starts of functions found exactly once in the destination,
    reduced to the longest run that increases in both."""
    index: dict[bytes, int] = {}
    seen_twice: set[bytes] = set()
    text = dst.text_end - dst.base
    for i in range(0, text - ANCHOR_BYTES + 1, 2):
        key = dst.code[i:i + ANCHOR_BYTES]
        if key in index:
            seen_twice.add(key)
        else:
            index[key] = i
    pairs = []
    for f in funcs:
        if f.size < ANCHOR_BYTES:
            continue
        key = src.code[f.addr - src.base:f.addr - src.base + ANCHOR_BYTES]
        if key in index and key not in seen_twice:
            pairs.append((f.addr, index[key] + dst.base))
    return longest_increasing(pairs)


def longest_increasing(pairs: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """The longest run of (a, b) pairs that increases in both, sorted by a."""
    pairs = sorted(pairs)
    tails: list[int] = []
    tail_idx: list[int] = []
    prev = [-1] * len(pairs)
    for i, (_, b) in enumerate(pairs):
        j = bisect.bisect_left(tails, b)
        if j == len(tails):
            tails.append(b)
            tail_idx.append(i)
        else:
            tails[j] = b
            tail_idx[j] = i
        prev[i] = tail_idx[j - 1] if j else -1
    out = []
    i = tail_idx[-1] if tail_idx else -1
    while i != -1:
        out.append(pairs[i])
        i = prev[i]
    return out[::-1]


def _containing(funcs: list[Function], starts: list[int], addr: int) -> Function | None:
    i = bisect.bisect_right(starts, addr & ~1) - 1
    if i >= 0 and (addr & ~1) < funcs[i].addr + funcs[i].size:
        return funcs[i]
    return None


def match_functions(src: Image, dst: Image, funcs: list[Function],
                    anchors: list[tuple[int, int]] | None = None) -> tuple[dict[int, int], dict]:
    """{source function: destination address} and a report. `anchors`: (source, destination)
    function starts known some other way (tools/code_signatures.py), instead of the bytes'."""
    funcs = sorted(funcs, key=lambda f: f.addr)
    anchors = _anchors(src, dst, funcs) if anchors is None else longest_increasing(anchors)
    anchor_src = [a for a, _ in anchors]
    mapped: dict[int, int] = {}
    pairs: dict[int, list[tuple[int, int]]] = {}
    last_start, last_shift = dst.base, 0   # (listed functions can overlap: order by start)
    for f in funcs:
        j = bisect.bisect_right(anchor_src, f.addr)
        cands = [last_shift]
        for k in (j - 1, j, j - 2, j + 1):
            if 0 <= k < len(anchors):
                d = anchors[k][1] - anchors[k][0]
                if d not in cands:
                    cands.append(d)
        for d in cands:
            if f.addr + d < last_start:
                continue
            p = compare(src, dst, f.addr, f.addr + d, f.size, f.thumb)
            if p is not None:
                mapped[f.addr], pairs[f.addr] = f.addr + d, p
                last_start, last_shift = f.addr + d, d
                break
    # every branch and pointer that was set aside must land on its target's new address
    starts = [f.addr for f in funcs]
    inconsistent = 0
    while True:
        bad = set()
        for a, ps in pairs.items():
            for tu, te in ps:
                g = _containing(funcs, starts, tu)
                if g is not None and g.addr in mapped and mapped[g.addr] - g.addr != (te & ~1) - (tu & ~1):
                    bad.add(a)
        if not bad:
            break
        inconsistent += len(bad)
        for a in bad:
            del mapped[a], pairs[a]
    checked = sum(1 for ps in pairs.values() for tu, _ in ps
                  if (g := _containing(funcs, starts, tu)) is not None and g.addr in mapped)
    report = {"functions": len(funcs), "anchors": len(anchors), "mapped": len(mapped),
              "unmapped": [f for f in funcs if f.addr not in mapped],
              "cross_checked": checked, "dropped_inconsistent": inconsistent, "pairs": pairs}
    return mapped, report


def text_intervals(src: Image, dst: Image, funcs: list[Function], mapped: dict[int, int]) -> list[tuple[int, int, int]]:
    funcs = sorted(funcs, key=lambda f: f.addr)
    intervals: list[tuple[int, int, int]] = []
    cur: list[int] | None = None   # [start, end, shift]
    for f in funcs:
        if f.addr not in mapped:
            if cur:   # a changed function ends the interval
                intervals.append(tuple(cur))
                cur = None
            continue
        d = mapped[f.addr] - f.addr
        if cur and cur[2] == d and _gap_same(src, dst, cur[1], f.addr, d):
            cur[1] = max(cur[1], f.addr + f.size)
            continue
        if cur:
            intervals.append(tuple(cur))
        cur = [f.addr, f.addr + f.size, d]
    if cur:
        intervals.append(tuple(cur))
    return intervals


def _gap_same(src: Image, dst: Image, start: int, end: int, d: int) -> bool:
    """Code between two functions with the same shift: unlisted functions, literal pools."""
    if end <= start:
        return True
    return (compare(src, dst, start, start + d, end - start, False) is not None
            or compare(src, dst, start, start + d, end - start, True) is not None)


# ── the data ───────────────────────────────────────────────────────────────

def data_evidence(src: Image, dst: Image, funcs: list[Function], mapped: dict[int, int]) -> dict[int, collections.Counter]:
    """{source data address: Counter of its shifts}: every aligned word of a matched function that
    points into the data in both versions (literal pools: the addresses the code uses)."""
    found: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    for f in funcs:
        if f.addr not in mapped:
            continue
        d = mapped[f.addr] - f.addr
        for a in range((f.addr + 3) & ~3, f.addr + f.size - 3, 4):
            wu, we = src.word(a), dst.word(a + d)
            if wu is not None and we is not None and src.in_data(wu) and dst.in_data(we) \
                    and abs(we - wu) < MAX_DATA_DELTA:
                found[wu][we - wu] += 1
    return found


def _split(src: Image, dst: Image, lo: int, hi: int, d1: int, d2: int) -> int | None:
    """Where in [lo, hi) the source data stops being at shift d1 and starts being at d2: the byte
    that agrees with the most bytes of both dumps (d1 before it, d2 from it on), or None when the
    bytes right around it don't say so (changed data, or no bytes)."""
    n = hi - lo
    if n <= 0:
        return None
    s0, a0, b0 = lo - src.base, lo + d1 - dst.base, lo + d2 - dst.base
    if s0 < 0 or a0 < 0 or b0 < 0 or s0 + n > len(src.code) or a0 + n > len(dst.code) or b0 + n > len(dst.code):
        return None
    s, d = src.code, dst.code
    m1 = [s[s0 + i] == d[a0 + i] for i in range(n)]
    m2 = [s[s0 + i] == d[b0 + i] for i in range(n)]
    best, best_k, before, after = -1, 0, 0, sum(m2)
    for k in range(n + 1):
        if before + after > best:
            best, best_k = before + after, k
        if k < n:
            before += m1[k]
            after -= m2[k]
    # the 8 bytes before the split must be at d1 and the 8 from it on at d2 (where there are such)
    if not all(m1[max(0, best_k - 8):best_k]) or not all(m2[best_k:best_k + 8]):
        return None
    return lo + best_k


def data_intervals(src: Image, dst: Image, evidence: dict[int, collections.Counter]) -> list[tuple[int, int, int]]:
    """Intervals [start, end) of the source data with one shift each, from the evidence: in each
    segment, runs of neighbouring addresses with the same shift, the first from the segment's start
    and the last to its end. A lone address whose shift differs from both its neighbours' (a number
    that looks like an address) is left out. Between two runs, where there are bytes, the boundary
    is where the bytes stop matching one shift and start matching the other; in .bss (or where the
    bytes don't tell) the addresses between the runs have no interval."""
    points = []
    for a, shifts in sorted(evidence.items()):
        d, n = shifts.most_common(1)[0]
        if n * 3 >= sum(shifts.values()) * 2:   # (a clear majority, else the address says nothing)
            points.append((a, d, sum(shifts.values())))
    out: list[tuple[int, int, int]] = []
    for seg_lo, seg_hi, bytes_hi in src.segments or ((src.data[0], src.data[1], src.initialized[1]),):
        runs: list[list[int]] = []   # [first, last, shift, weight]
        for a, d, n in points:
            if not seg_lo <= a < seg_hi:
                continue
            if runs and runs[-1][2] == d:
                runs[-1][1], runs[-1][3] = a, runs[-1][3] + n
            else:
                runs.append([a, a, d, n])
        # a lone, weakly held point between two runs of one other shift is noise
        changed = True
        while changed:
            changed = False
            for i in range(1, len(runs) - 1):
                if runs[i][0] == runs[i][1] and runs[i][3] <= 1 and runs[i - 1][2] == runs[i + 1][2] != runs[i][2]:
                    runs[i - 1][1], runs[i - 1][3] = runs[i + 1][1], runs[i - 1][3] + runs[i + 1][3]
                    del runs[i:i + 2]
                    changed = True
                    break
        start = seg_lo
        for i, (first, last, d, _) in enumerate(runs):
            if i + 1 == len(runs):
                out.append((start, seg_hi, d))
                break
            nfirst, nd = runs[i + 1][0], runs[i + 1][2]
            boundary = _split(src, dst, last + 1, min(nfirst, bytes_hi), d, nd) if last + 1 < bytes_hi else None
            if boundary is not None:
                out.append((start, boundary, d))
                start = boundary
            else:
                out.append((start, last + 4, d))   # (the address the code points to and the word after it)
                start = nfirst
    return [(s, e, d) for s, e, d in out if e > s]


def build_shifts(src: Image, dst: Image, funcs: list[Function],
                 anchors: list[tuple[int, int]] | None = None) -> tuple[Shifts, dict]:
    mapped, report = match_functions(src, dst, funcs, anchors)
    data, dints = None, []
    if src.data != (0, 0):
        evidence = data_evidence(src, dst, funcs, mapped)
        dints = data_intervals(src, dst, evidence)
        moved = any(d != 0 for _, _, d in dints) or sum(e - s for s, e, _ in dints) < sum(
            e - s for s, e, _ in (src.segments or ((src.data[0], src.data[1], 0),)))
        if moved:
            data = src.data
            # a matched function whose data pointers land somewhere else is not the same function
            probe = Shifts([], None, None, dints, src.data)
            dropped = 0
            for a, ps in list(report["pairs"].items()):
                if any(src.in_data(tu) and probe.translate(tu) != te for tu, te in ps):
                    del mapped[a]
                    dropped += 1
            report["dropped_data"] = dropped
            report["mapped"] = len(mapped)
            report["unmapped"] = [f for f in funcs if f.addr not in mapped]
        report["data_points"] = len(evidence)
        report["data_intervals"] = len(dints) if moved else 0
    intervals = text_intervals(src, dst, funcs, mapped)
    report["intervals"] = len(intervals)
    return Shifts(intervals, (src.base, src.text_end), None, dints if data else None, data), report


def load_image(exheader_path: str, code_path: str) -> tuple[Image, exh.ExHeader, bytes]:
    with open(exheader_path, "rb") as f:
        header = exh.parse(f.read())
    with open(code_path, "rb") as f:
        code = codebin2elf.load_code(header, f.read())
    return image_of(header, code), header, code


def image_of(header: exh.ExHeader, code: bytes) -> Image:
    ro_end, data_end = header.ro.address + header.ro.size, header.data.address + header.data.size
    return Image(header.text.address, code, header.text.address + header.text.size,
                 (header.ro.address, data_end + header.bss_size), (header.ro.address, data_end),
                 ((header.ro.address, ro_end, ro_end), (header.data.address, data_end + header.bss_size, data_end)))


def same_layout(a: exh.ExHeader, b: exh.ExHeader) -> list[str]:
    """What, besides .text's size, differs between the two layouts (empty when their data is laid out alike)."""
    out = []
    for name in ("ro", "data"):
        sa, sb = getattr(a, name), getattr(b, name)
        if (sa.address, sa.size) != (sb.address, sb.size):
            out.append(f".{name}: 0x{sa.address:08x}+0x{sa.size:x} vs 0x{sb.address:08x}+0x{sb.size:x}")
    if a.text.address != b.text.address:
        out.append(f".text base: 0x{a.text.address:08x} vs 0x{b.text.address:08x}")
    if a.bss_size != b.bss_size:
        out.append(f".bss size: 0x{a.bss_size:x} vs 0x{b.bss_size:x}")
    return out


def load_functions(path: str) -> list[Function]:
    return [Function(int(r["address"], 16), int(r["size"], 16), r["mode"] == "thumb")
            for r in bft.read_tsv(path)]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="map the source version's functions into the destination's code")
    b.add_argument("--src-exheader", required=True)
    b.add_argument("--src-code", required=True)
    b.add_argument("--dst-exheader", required=True)
    b.add_argument("--dst-code", required=True)
    b.add_argument("--functions", default=os.path.join(os.path.dirname(__file__), "..", "config", "functions.tsv"))
    b.add_argument("--src-name", default="USA v1.3.0")
    b.add_argument("--name", required=True, help="the destination version, for the file's header")
    b.add_argument("-o", "--out", required=True)
    t = sub.add_parser("translate", help="print the destination address of source addresses")
    t.add_argument("shifts")
    t.add_argument("addresses", nargs="+")
    args = ap.parse_args(argv)

    if args.cmd == "translate":
        shifts = Shifts.load(args.shifts)
        for a in args.addresses:
            r = shifts.translate(int(a, 16))
            print(f"0x{int(a, 16):08x} -> " + ("unmapped" if r is None else f"0x{r:08x}"))
        return 0

    src, src_h, src_code = load_image(args.src_exheader, args.src_code)
    dst, dst_h, dst_code = load_image(args.dst_exheader, args.dst_code)
    problems = same_layout(src_h, dst_h)
    if problems:
        print("the two versions' data differ (" + "; ".join(problems) + "): the map gets a data section")
    shifts, report = build_shifts(src, dst, load_functions(args.functions))
    shifts.header = [
        f"Shifts from {args.src_name} (code sha1 {hashlib.sha1(src_code).hexdigest()}) "
        f"to {args.name} (code sha1 {hashlib.sha1(dst_code).hexdigest()}).",
        "Built by tools/version_map.py from both dumps; addresses only. Each line: <start> <end> <shift>:",
        "source addresses in [start, end) are at address + shift. The text section covers .text;" + (
            " the data section .rodata to the end of .bss (an address there in no interval has no translation)."
            if shifts.data else " outside .text, addresses are the same."),
        f"{report['mapped']}/{report['functions']} listed functions mapped, "
        f"{report['cross_checked']} branches and code pointers cross-checked"
        + (f", {report['data_points']} data addresses used by the code compared." if shifts.data else "."),
    ]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(shifts.dumps())
    print(f"{report['mapped']}/{report['functions']} functions mapped ({report['anchors']} anchors), "
          f"{report['intervals']} intervals; {report['cross_checked']} branches/pointers cross-checked, "
          f"{report['dropped_inconsistent']} functions dropped as inconsistent"
          + (f"; data: {report['data_points']} addresses, {report['data_intervals']} intervals, "
             f"{report.get('dropped_data', 0)} functions dropped for their data pointers" if shifts.data else ""))
    for f in report["unmapped"]:
        print(f"  unmapped: 0x{f.addr:08x} size 0x{f.size:x} {'thumb' if f.thumb else 'arm'}")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
