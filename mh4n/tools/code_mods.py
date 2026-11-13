#!/usr/bin/env python3
"""Community code mods for the game's code.bin, applied the way Azahar applies them.

Azahar loads `load/mods/<program id>/exefs/code.bin` (or `.../code.bin`) in place of the
game's code, then patches it with the first of `exefs/code.ips`, `exefs/code.bps`,
`code.ips`, `code.bps` that exists (src/core/file_sys/ncch_container.cpp, patch.cpp; the
program id of an update counts as the base game's). The port's code library is the game's
code recompiled, so it has to be built from the same modded code: a mod that changes the
code (.text) makes the running code differ from the recompiled one, and the port then
interprets everything. A mod that only changes data (.rodata/.data) needs no rebuild.

    python3 tools/code_mods.py AZAHAR_USER_DIR 0004000000126300 code.bin   # which mod applies
"""
from __future__ import annotations

import os
import re
import sys
import zlib
from dataclasses import dataclass, field


class ModError(ValueError):
    pass


def apply_ips(code: bytes, patch: bytes) -> bytes:
    """IPS: "PATCH", records of (3-byte offset, 2-byte length, data) or, with length 0, an RLE
    run (2-byte count, 1 byte), then "EOF". Like Azahar, a record may not write past the end
    of the code, and anything after "EOF" is ignored."""
    if patch[:5] != b"PATCH":
        raise ModError("not an IPS patch")
    out = bytearray(code)
    cursor, end = 5, len(patch) - 3
    while cursor < end:
        if patch[cursor:cursor + 3] == b"EOF":
            break
        offset = int.from_bytes(patch[cursor:cursor + 3], "big")
        length = int.from_bytes(patch[cursor + 3:cursor + 5], "big")
        if length == 0:
            count = int.from_bytes(patch[cursor + 5:cursor + 7], "big")
            if offset + count > len(out):
                raise ModError(f"IPS run at {offset:#x} goes past the end of the code")
            out[offset:offset + count] = patch[cursor + 7:cursor + 8] * count
            cursor += 8
            continue
        if offset + length > len(out):
            raise ModError(f"IPS record at {offset:#x} goes past the end of the code")
        out[offset:offset + length] = patch[cursor + 5:cursor + 5 + length]
        cursor += 5 + length
    return bytes(out)


def _bps_number(patch: bytes, pos: int) -> tuple[int, int]:
    data, shift = 0, 1
    while pos < len(patch):
        x = patch[pos]
        pos += 1
        data += (x & 0x7F) * shift
        if x & 0x80:
            break
        shift <<= 7
        data += shift
    return data, pos


def apply_bps(code: bytes, patch: bytes) -> bytes:
    """BPS ("BPS1"), checked by its source and target CRC32s. As in Azahar, no metadata, and
    the result keeps the code's length when the target is shorter."""
    if patch[:4] != b"BPS1" or len(patch) < 16:
        raise ModError("not a BPS patch")
    source_size, pos = _bps_number(patch, 4)
    target_size, pos = _bps_number(patch, pos)
    metadata_size, pos = _bps_number(patch, pos)
    if source_size > len(code) or metadata_size != 0:
        raise ModError("BPS sizes don't fit this code (or the patch has metadata)")
    end = len(patch) - 12
    source_crc = int.from_bytes(patch[end:end + 4], "little")
    target_crc = int.from_bytes(patch[end + 4:end + 8], "little")
    if zlib.crc32(code[:source_size]) != source_crc:
        raise ModError("this BPS patch is for another code.bin (source CRC32 differs)")
    target = bytearray(max(len(code), target_size))
    out = source_rel = target_rel = 0
    while pos < end:
        data, pos = _bps_number(patch, pos)
        command, length = data & 3, (data >> 2) + 1
        if out + length > len(target):
            raise ModError("BPS patch writes past the end")
        if command == 0:      # SourceRead
            target[out:out + length] = code[out:out + length]
        elif command == 1:    # TargetRead
            target[out:out + length] = patch[pos:pos + length]
            pos += length
        elif command == 2:    # SourceCopy
            delta, pos = _bps_number(patch, pos)
            source_rel += -(delta >> 1) if delta & 1 else delta >> 1
            if source_rel < 0 or source_rel + length > len(code):
                raise ModError("BPS source copy out of range")
            target[out:out + length] = code[source_rel:source_rel + length]
            source_rel += length
        else:                 # TargetCopy, byte by byte (it may overlap what it writes)
            delta, pos = _bps_number(patch, pos)
            target_rel += -(delta >> 1) if delta & 1 else delta >> 1
            if target_rel < 0 or target_rel + length > len(target):
                raise ModError("BPS target copy out of range")
            for i in range(length):
                target[out + i] = target[target_rel]
                target_rel += 1
        out += length
    if zlib.crc32(bytes(target[:target_size])) != target_crc:
        raise ModError("BPS result doesn't match the patch's target CRC32")
    return bytes(target)


def mod_dir(user_dir: str, program_id: int) -> str:
    """Azahar's mod folder for a title: an update's mods are the base game's."""
    if program_id & 0x000000FF_00000000 == 0x0000000E_00000000:
        program_id &= ~0x0000000E_00000000
    return os.path.join(user_dir, "load", "mods", f"{program_id:016X}")


def find_mods(user_dir: str, program_id: int) -> tuple[str | None, str | None]:
    """(replacement code.bin, patch) that Azahar would use for this title, or None."""
    base = mod_dir(user_dir, program_id)
    replacement = next((p for p in (os.path.join(base, "exefs", "code.bin"), os.path.join(base, "code.bin"))
                        if os.path.isfile(p)), None)
    patch = next((p for p in (os.path.join(base, "exefs", "code.ips"), os.path.join(base, "exefs", "code.bps"),
                              os.path.join(base, "code.ips"), os.path.join(base, "code.bps"))
                  if os.path.isfile(p)), None)
    return replacement, patch


MAX_BSS_SIZE = 0x10000000      # the New 3DS FCRAM size


def bss_pages_size(bss_size: int) -> int:
    """The bytes Azahar appends to the code for an exheader's bss size: (bss_size + 0xFFF) & ~0xFFF.
    The size is a u32 as in Azahar; one above 0x10000000 (the New 3DS FCRAM) is a ModError."""
    bss_size &= 0xFFFFFFFF
    if bss_size > MAX_BSS_SIZE:
        raise ModError(f"the exheader's bss size 0x{bss_size:X} is larger than the console's memory")
    return (bss_size + 0xFFF) & ~0xFFF


def apply_mods(code: bytes, user_dir: str, program_id: int, bss_size: int = 0) -> tuple[bytes, list[str]]:
    """The code as Azahar will run it with this user folder's mods, and the files used.

    `bss_size` is the exheader's bss size (after any exheader override); the default 0 keeps the old
    behaviour (no bss pages) for callers that don't pass it. As in Azahar's AppLoader_NCCH::LoadExec (D79)
    a replacement code.bin is loaded first, then the pages (bss_size + 0xFFF) & ~0xFFF of zeros are
    appended, then the first patch is applied, so a patch may read and write the pages. The code returned
    is without the pages (the code library is built from the code only): the first len(code) bytes of the
    patched buffer (code = the game's, or the replacement's), zero-padded if the buffer came out shorter
    (a BPS can resize it); what the patch wrote into the pages is dropped. Without pages the patched
    buffer is returned whole, as before, and a mod that doesn't touch the pages gives the bytes it always gave."""
    replacement, patch = find_mods(user_dir, program_id)
    used = []
    if replacement:
        with open(replacement, "rb") as f:
            code = f.read()
        used.append(replacement)
    if patch:
        pages = bss_pages_size(bss_size)
        with open(patch, "rb") as f:
            data = f.read()
        buffer = code + bytes(pages)
        buffer = apply_ips(buffer, data) if patch.endswith(".ips") else apply_bps(buffer, data)
        code = buffer if pages == 0 else buffer[:len(code)].ljust(len(code), b"\0")
        used.append(patch)
    return code, used


# ── the native app's mod list (mh4n native/src/mods/mod_list.h) ───────────────
# The native app reads every folder of <user>/load/mods as a mod, in the order of load/mods/load_order.txt, not
# only Azahar's load/mods/<program id>/. These mirror mod_list.cpp and code_patch::ApplyFolders, so that a library
# made here is made from the code the app will run.

ORDER_FILE = "load_order.txt"
OFF_PREFIX = "# off: "
CODE_FILES = ("exefs/code.ips", "exefs/code.bps", "exefs/code.bin", "code.ips", "code.bps", "code.bin")
REPLACEMENTS = ("exefs/code.bin", "code.bin")
PATCHES = ("exefs/code.ips", "exefs/code.bps", "code.ips", "code.bps")
EXHEADER = "exheader.bin"
EXHEADER_SIZE = 0x800


@dataclass
class Mod:
    name: str
    path: str
    on: bool = True
    code: bool = False       # a code replacement or patch, or an exheader (it goes with a code replacement)
    files: list[str] = field(default_factory=list)   # which of CODE_FILES and exheader.bin it has


def _is_title_id(name: str) -> bool:
    return re.fullmatch(r"[0-9A-Fa-f]{16}", name) is not None


def mod_id(program_id: int) -> int:
    """An update's program id counts as the base game's (Azahar's GetModId)."""
    if program_id & 0x000000FF_00000000 == 0x0000000E_00000000:
        program_id &= ~0x0000000E_00000000
    return program_id


def describe_mod(folder: str, name: str) -> Mod:
    path = os.path.join(folder, name)
    files = [rel for rel in CODE_FILES if os.path.isfile(os.path.join(path, rel))]
    if os.path.isfile(os.path.join(path, EXHEADER)):
        files.append(EXHEADER)
    return Mod(name, path, True, bool(files), files)


def list_mods(mods_folder: str, program_id: int) -> list[Mod]:
    """Every mod of the title in mods_folder (<user>/load/mods), in load order, as mods::List: load_order.txt's
    order ("# off: name" keeps a mod that is off in its place), then the folders it doesn't name, on, by name. A
    folder named as another title's id, or whose name starts with '.' or '_', is not one."""
    if not os.path.isdir(mods_folder):
        return []
    own = f"{mod_id(program_id):016X}"
    names = sorted(n for n in os.listdir(mods_folder)
                   if os.path.isdir(os.path.join(mods_folder, n)) and n and n[0] not in "._"
                   and (not _is_title_id(n) or n.upper() == own))
    out, placed = [], set()
    order = os.path.join(mods_folder, ORDER_FILE)
    if os.path.isfile(order):
        with open(order, encoding="utf-8", errors="replace") as f:
            for line in f:
                line, on = line.strip(), True
                if line.startswith(OFF_PREFIX):
                    line, on = line[len(OFF_PREFIX):].strip(), False
                elif not line or line.startswith("#"):
                    continue
                if line in placed or line not in names:
                    continue
                m = describe_mod(mods_folder, line)
                m.on = on
                out.append(m)
                placed.add(line)
    out += [describe_mod(mods_folder, n) for n in names if n not in placed]
    return out


def code_folders(mods: list[Mod]) -> list[str]:
    """The folders of the code mods that are on, in load order."""
    return [m.path for m in mods if m.on and m.code]


def _first(folder: str, names: tuple[str, ...]) -> str | None:
    return next((os.path.join(folder, rel) for rel in names if os.path.isfile(os.path.join(folder, rel))), None)


def apply_folders(code: bytes, folders: list[str], bss_size: int = 0) -> tuple[bytes, list[str]]:
    """code_patch::ApplyFolders: the code replacement of the last folder that has one, then the bss pages, then
    each folder's first patch in turn. Returns the code without the pages (as apply_mods) and the files used."""
    used = []
    for folder in reversed(folders):
        replacement = _first(folder, REPLACEMENTS)
        if replacement:
            with open(replacement, "rb") as f:
                code = f.read()
            used.append(replacement)
            break
    pages = bss_pages_size(bss_size)
    buffer = code + bytes(pages)
    for folder in folders:
        patch = _first(folder, PATCHES)
        if patch:
            with open(patch, "rb") as f:
                data = f.read()
            try:
                buffer = apply_ips(buffer, data) if patch.endswith(".ips") else apply_bps(buffer, data)
            except ModError as e:
                raise ModError(f"{os.path.basename(folder)}: {e}") from None
            used.append(patch)
    out = buffer if pages == 0 else buffer[:len(code)].ljust(len(code), bytes(1))
    return out, used


def exheader_override(folders: list[str]) -> tuple[str, bytes] | None:
    """The exheader.bin that takes the game's place (code_patch::ExheaderOverride; with several folders, the last
    folder's that has one, as with the code replacement): its path and first 0x800 bytes, or None."""
    for folder in reversed(folders):
        path = os.path.join(folder, EXHEADER)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                data = f.read()
            if len(data) >= EXHEADER_SIZE:
                return path, data[:EXHEADER_SIZE]
    return None


def ips_ranges(patch: bytes) -> list[tuple[int, int]]:
    """The [start, end) ranges an IPS patch writes (records and runs), or [] for one that isn't an IPS."""
    if patch[:5] != b"PATCH":
        return []
    out, cursor, end = [], 5, len(patch) - 3
    while cursor < end and patch[cursor:cursor + 3] != b"EOF":
        offset = int.from_bytes(patch[cursor:cursor + 3], "big")
        length = int.from_bytes(patch[cursor + 3:cursor + 5], "big")
        if length == 0:
            count = int.from_bytes(patch[cursor + 5:cursor + 7], "big")
            out.append((offset, offset + count))
            cursor += 8
        else:
            out.append((offset, offset + length))
            cursor += 5 + length
    return out


def code_conflicts(mods: list[Mod]) -> list[tuple[str, str, str]]:
    """Code mods that are on and don't go together (mods::CodeConflicts in mh4n native/src/mods, the same rules):
    (kind, mod, other mod), kinds as CONFLICT_TEXT. The replacement is applied first and every patch after it, so a
    patch in another mod than the one whose code replaces the game's lands on that mod's code, whatever the order."""
    on = [m for m in mods if m.on and m.code]
    out: list[tuple[str, str, str]] = []
    replacing = [m for m in on if any(f in m.files for f in REPLACEMENTS)]
    for a, b in zip(replacing, replacing[1:]):
        out.append(("replaced", a.name, b.name))
    last = replacing[-1] if replacing else None
    for m in on:
        if last is not None and m is not last and any(f in m.files for f in PATCHES):
            out.append(("patch_on_replacement", m.name, last.name))
    with_exheader = [m for m in on if EXHEADER in m.files]
    if with_exheader and last is not None and with_exheader[-1] is not last:
        out.append(("exheader", with_exheader[-1].name, last.name))
    patched: list[tuple[str, list[tuple[int, int]] | None]] = []
    for m in on:
        patch = _first(m.path, PATCHES)
        if patch is None:
            continue
        if patch.endswith(".bps"):
            if patched:
                out.append(("bps", m.name, patched[-1][0]))
            patched.append((m.name, None))
            continue
        with open(patch, "rb") as f:
            ranges = ips_ranges(f.read())
        for other, other_ranges in patched:
            if other_ranges is not None and any(s < oe and os_ < e for s, e in ranges for os_, oe in other_ranges):
                out.append(("overlap", m.name, other))
        patched.append((m.name, ranges))
    return out


CONFLICT_TEXT = {
    "replaced": "{a} and {b} both replace the game's code: only {b}'s is used",
    "patch_on_replacement": "{a} patches the game's code, but {b} replaces that code: {a}'s patch was made for the "
                            "game's code, not for {b}'s, and breaks it (turn one of them off)",
    "exheader": "{a}'s exheader.bin goes with its own code, but the code that runs is {b}'s",
    "overlap": "{a} and {b} patch the same bytes of the code: the later one wins there and the other one breaks",
    "bps": "{a}'s BPS patch was made for the game's code, but {b}'s patch changes that code first: it won't apply",
}


def describe_conflict(kind: str, a: str, b: str) -> str:
    return CONFLICT_TEXT[kind].format(a=a, b=b)


def game_folder_mods(mods_dir: str, program_id: int) -> dict[str, str]:
    """The mods of a game folder's mods/ as the app copies them into load/mods (GameFolderActivity.syncMods):
    {name in load/mods: folder}. mods/<program id>/ (or the update's id) is Azahar's layout and keeps that name;
    mods/<name>/<program id>/, and mods/<name>/ with romfs/, romfs_ext/ or exefs/ in it, are <name>."""
    base = f"{mod_id(program_id):016X}"
    update = f"{mod_id(program_id) | 0x0000000E_00000000:016X}"

    def title_in(folder: str) -> str | None:
        kids = {k.upper(): os.path.join(folder, k) for k in os.listdir(folder) if os.path.isdir(os.path.join(folder, k))}
        return kids.get(base) or kids.get(update)

    out: dict[str, str] = {}
    if not os.path.isdir(mods_dir):
        return out
    title = title_in(mods_dir)
    if title:
        out[base] = title
    for k in sorted(os.listdir(mods_dir)):
        path = os.path.join(mods_dir, k)
        if not os.path.isdir(path) or _is_title_id(k):
            continue
        src = title_in(path)
        if src is None and any(os.path.isdir(os.path.join(path, part)) for part in ("romfs", "romfs_ext", "exefs")):
            src = path
        if src is not None:
            out[k] = src
    return out


def changed_bytes(old: bytes, new: bytes, start: int, size: int) -> int:
    """How many bytes of [start, start + size) differ (bytes past either end count)."""
    a, b = old[start:start + size], new[start:start + size]
    return sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))


def describe(old: bytes, new: bytes, segments: list[tuple[str, int, int]]) -> list[str]:
    """Per segment (name, offset in the code, size): how many bytes the mods change."""
    return [f"{name}: {changed_bytes(old, new, offset, size)} bytes changed" for name, offset, size in segments]


def main(argv=None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 3:
        print("usage: code_mods.py AZAHAR_USER_DIR PROGRAM_ID_HEX code.bin", file=sys.stderr)
        return 1
    with open(args[2], "rb") as f:
        code = f.read()
    new, used = apply_mods(code, args[0], int(args[1], 16))
    if not used:
        print(f"no code mods in {mod_dir(args[0], int(args[1], 16))}")
        return 0
    print("\n".join(used))
    print(f"{changed_bytes(code, new, 0, max(len(code), len(new)))} bytes changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
