#!/usr/bin/env python3
"""Builds the game's code library ON THE PHONE, for the native Android app's builder (Settings, Game folder, "Build on
this phone"): the same tools/build_game.py the PC scripts beside this file run, with the same arguments
(`--native --android`, the base game's shaders with --romfs, the code mod staged as the PC scripts stage it), so that
whatever the pipeline accepts and checks on a PC it accepts and checks here too.

    python3 phone_build.py --update UPDATE.cia [--game GAME.cci] [--game-mods FOLDER | --user-mods USER | --mod FOLDER]
                           --work DIR --dest FOLDER --cc COMPILER [--jobs N] [--expect-id ID] [--clean]

The code mods (they have to be the ones the game will run with): --game-mods, the game folder's mods/ as the app copies
it to the phone's user folder (the first build: nothing is copied yet); --user-mods, the app's user folder, whose
load/mods the game runs (every mod that is on, in load_order.txt's order); --mod, one mod's folder. Tools that know
build_game.py's --game-mods take the first two as they are (every code mod, a mod's exheader.bin...); older ones
build in the first code mod found, as the PC scripts did.

The app runs it with the CPython for Android it downloaded (python.org) and Zig's C compiler (PyPI's ziglang 0.13.0,
the one build_game.py fetches on Linux), through its two small launchers (app_native/builder/toolrun.c): --cc is the
launcher libzigcc.so in the app's native library folder.

Besides build_game.py's own output it prints lines for the app, each starting with "@@":
    @@id ID                     the code library id these tools give (the app compares it with its own first)
    @@stage NAME                extract, version, mods, recompile, shaders, compile, link, install
    @@progress NAME DONE TOTAL  how far a stage is
    @@error NAME MESSAGE        the stage that failed and why (the last line of a failed run)
    @@done PATH                 the library is in place, with its .id beside it

Resuming: the work folder stays between runs. The recompiled C is written only where it changed and an object is
compiled again only when its C is newer (tools/recomp/build.py), so a run cut short (the app closed, the phone too hot,
no battery) goes on from the objects already made; an object the cut left half written is found here and made again.
The library reaches --dest only whole (written beside it, then renamed), with its .id.
The stages before the compile (the extraction, the code mods, the analysis and the translation to C with its function
list, the geometry shaders: minutes of every core on a phone) are done again only when what they are made from changed:
once they are through, the work folder's inputs.txt keeps a hash of their inputs (the update and the game, the code
mods, these tools and the Python that runs them) and a list of the C they wrote; a run whose inputs give the same hash,
with that C still there, goes straight to compiling what is missing (build_game.py --compile-only). The C is the same as
a run from an empty work folder writes, and so is every object, save one: Zig 0.13's clang compiles one very large
function of the game code (f_00ce7920 with the single-screen mod) into one of a few objects from one run to the next,
cut or not (the app's launcher, toolrun.c, makes that less likely).

Everything this writes is made from the player's own game and stays on the phone.
"""
from __future__ import annotations

import argparse
import errno
import hashlib
import io
import os
import re
import shutil
import struct
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import build_game  # noqa: E402

TITLE = "0004000000126300"
LIBRARY = "libmh4u_code_native.so"
# room the work folder needs: the recompiled C (about 330 MB), its objects (140 MB) and Zig's copies of them (140 MB)
WORK_SPACE = 800 << 20


def emit(line: str) -> None:
    sys.__stdout__.write(line + "\n")
    sys.__stdout__.flush()


# ---- the stages before the compile, kept between runs (the module's docstring; the MHGU port's phone_build.py the same)
STAMP = "inputs.txt"
WHOLE = 128 << 20      # a file up to this size is hashed whole (the update: 48 MB, under a second on a phone)...
END = 1 << 20          # ...a larger one (the game's .3ds: GBs, a minute to read) by its size, its date and its two ends
SMALL = 64 << 10       # in a mods folder: the files the code mods are made of, and small ones (an order, a switch),
CODE_NAMES = {"code.bin", "code.ips", "code.bps", "exheader.bin"}  # whole; the rest (romfs) by their sizes
# the tools as the app packs them (its scripts/pack_builder_tools.py: tools/ and config/ without the PC's research)
TOOL_TOPS = ("tools", "config")
TOOL_SKIP = {"__pycache__", "ghidra", "native", "harness", "tests"}


def file_digest(path: str) -> str:
    st = os.stat(path)
    h = hashlib.sha256()
    with open(path, "rb") as f:
        if st.st_size <= WHOLE:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
            return f"{st.st_size} {h.hexdigest()}"
        h.update(f.read(END))
        f.seek(-END, os.SEEK_END)
        h.update(f.read(END))
    return f"{st.st_size} {st.st_mtime_ns} {h.hexdigest()}"


def folder_digest(top: str | None, whole=lambda name, size: True, skip_dirs=(), skip_ext=()) -> str:
    """Every folder and file under top (relative names, in order): a file's bytes when whole(name, size), else its size."""
    if not top or not os.path.isdir(top):
        return "none"
    h = hashlib.sha256()
    for root, dirs, names in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d not in skip_dirs)
        rel = os.path.relpath(root, top).replace(os.sep, "/")
        h.update(f"d {rel}\n".encode("utf-8", "surrogateescape"))
        for n in sorted(names):
            if skip_ext and n.endswith(skip_ext):
                continue
            path = os.path.join(root, n)
            size = os.path.getsize(path)
            h.update(f"f {rel}/{n} {file_digest(path) if whole(n, size) else size}\n".encode("utf-8", "surrogateescape"))
    return h.hexdigest()


def mods_digest(top: str | None) -> str:
    return folder_digest(top, lambda name, size: name.lower() in CODE_NAMES or size <= SMALL)


def tools_digest() -> str:
    h = hashlib.sha256()
    for top in TOOL_TOPS:
        d = folder_digest(os.path.join(build_game.ROOT, top), skip_dirs=TOOL_SKIP, skip_ext=(".pyc", ".exe", ".dll", ".so"))
        h.update(f"{top} {d}\n".encode())
    return h.hexdigest()


def any_digest(path: str | None) -> str:
    return file_digest(path) if path and os.path.isfile(path) else "none"


def stamp_hash(lines: list[str]) -> str:
    return hashlib.sha256("\n".join(lines).encode("utf-8", "surrogateescape")).hexdigest()


def written(out: str) -> str:
    """What the stages before the compile left for it: the C (out/recomp/src, names and sizes) and the dump's header,
    which names the library."""
    h = hashlib.sha256()
    src = os.path.join(out, "recomp", "src")
    names = sorted(os.listdir(src)) if os.path.isdir(src) else []
    if "funcs.h" not in names or "table.c" not in names:
        return "none"
    for n in names:
        h.update(f"{n} {os.path.getsize(os.path.join(src, n))}\n".encode("utf-8", "surrogateescape"))
    exheader = os.path.join(out, "dump", "exheader.bin")
    h.update(file_digest(exheader).encode() if os.path.isfile(exheader) else b"no exheader")
    return h.hexdigest()


def read_stamp(work: str) -> tuple[str, str]:
    try:
        with open(os.path.join(work, STAMP), encoding="utf-8", errors="replace") as f:
            first = f.readline().split()
        return (first[0], first[1]) if len(first) >= 2 else ("", "")
    except OSError:
        return "", ""


def write_stamp(work: str, inputs: str, made: str, lines: list[str]) -> None:
    path = os.path.join(work, STAMP)
    with open(path + ".part", "w", encoding="utf-8", errors="replace") as f:
        f.write(f"{inputs} {made}\n" + "\n".join(lines) + "\n")
    os.replace(path + ".part", path)


def drop_stamp(work: str) -> None:
    try:
        os.remove(os.path.join(work, STAMP))
    except OSError:
        pass


class Stages(io.TextIOBase):
    """build_game.py's output, passed on as it is, with the app's @@ lines for the stages it goes through."""

    PATTERNS = [
        (re.compile(r"^extracting "), "extract"),
        (re.compile(r"^version: "), "version"),
        (re.compile(r"^code mods: "), "mods"),
        (re.compile(r"^recompiling"), "recompile"),
        (re.compile(r"^geometry shaders"), "shaders"),
        (re.compile(r"^compiling with "), "compile"),
        (re.compile(r"^linked "), "link"),
    ]
    ANALYSED = re.compile(r"analysed (\d+)/(\d+)")

    def __init__(self):
        super().__init__()
        self.stage = "extract"
        self.buffer_ = ""
        self.on_stage = None

    def writable(self):
        return True

    def write(self, s):
        sys.__stdout__.write(s)
        self.buffer_ += s
        while "\n" in self.buffer_:
            line, self.buffer_ = self.buffer_.split("\n", 1)
            self.line(line.strip())
        return len(s)

    def flush(self):
        sys.__stdout__.flush()

    def line(self, text):
        for pattern, stage in self.PATTERNS:
            if pattern.search(text) and stage != self.stage:
                self.set(stage)
        m = self.ANALYSED.search(text)
        if m:
            emit(f"@@progress recompile {m.group(1)} {m.group(2)}")

    def set(self, stage):
        self.stage = stage
        emit(f"@@stage {stage}")
        if self.on_stage:
            self.on_stage(stage)


class Tail(io.TextIOBase):
    """stderr passed on as it is, its last 64 KB kept (a compiler's error, read after a failed compile)."""

    def __init__(self):
        super().__init__()
        self.text = ""

    def writable(self):
        return True

    def write(self, s):
        sys.__stderr__.write(s)
        self.text = (self.text + s)[-(64 << 10):]
        return len(s)

    def flush(self):
        sys.__stderr__.flush()


def whole_object(path: str) -> bool:
    """An ELF object whose section headers are all inside the file: one cut short while it was written isn't."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            head = f.read(64)
        if len(head) < 64 or head[:4] != b"\x7fELF" or head[4] != 2:
            return False
        shoff = struct.unpack_from("<Q", head, 40)[0]
        shentsize, shnum = struct.unpack_from("<HH", head, 58)
        return shoff > 0 and shoff + shentsize * shnum <= size
    except OSError:
        return False


def drop_broken_objects(obj_dir: str) -> int:
    dropped = 0
    if os.path.isdir(obj_dir):
        for name in os.listdir(obj_dir):
            path = os.path.join(obj_dir, name)
            if name.endswith(".o") and not whole_object(path):
                os.remove(path)
                dropped += 1
    return dropped


class CompileProgress(threading.Thread):
    """While compiling: how many of the C files have their object (build.py prints only every 20 files)."""

    def __init__(self, recomp_dir: str):
        super().__init__(daemon=True)
        self.src = os.path.join(recomp_dir, "src")
        self.obj = os.path.join(recomp_dir, "obj")
        self.stop = threading.Event()

    def run(self):
        last = None
        while not self.stop.wait(3):
            try:
                sources = [f[:-2] for f in os.listdir(self.src) if f.endswith(".c")]
                done = 0
                objects = os.listdir(self.obj) if os.path.isdir(self.obj) else []
                for s in sources:
                    src_time = os.path.getmtime(os.path.join(self.src, s + ".c"))
                    if any(o.startswith(s + "-") and o.endswith(".o") and os.path.getmtime(os.path.join(self.obj, o)) >= src_time
                           for o in objects):
                        done += 1
                now = (done, len(sources) + 1)  # (+1: rc_dll.c, the glue)
                if now != last:
                    emit(f"@@progress compile {now[0]} {now[1]}")
                    last = now
            except OSError:
                pass


def stage_mod(mod: str, user_dir: str) -> str:
    """The code mod as build_game.py reads it (load/mods/<title>, Azahar's layout), from the folder the player chose: a
    mods folder with 0004000000126300 in it (or a mod's folder with it inside), or the mod's own folder (with exefs):
    what the PC scripts beside this file take."""
    title = None
    for depth_dir in [mod] + sorted(os.path.join(mod, d) for d in os.listdir(mod) if os.path.isdir(os.path.join(mod, d))):
        for name in sorted(os.listdir(depth_dir)) if os.path.isdir(depth_dir) else []:
            if re.fullmatch(r"0004000[0eE]00126[13]00", name) and os.path.isdir(os.path.join(depth_dir, name)):
                title = os.path.join(depth_dir, name)
                break
        if title:
            break
    if title is None and os.path.isdir(os.path.join(mod, "exefs")):
        title = mod
    if title is None:
        raise SystemExit(f"there is no {TITLE} or exefs folder in {mod}")
    shutil.rmtree(user_dir, ignore_errors=True)
    dest = os.path.join(user_dir, "load", "mods", TITLE)
    os.makedirs(os.path.dirname(dest))
    shutil.copytree(title, dest)
    return title


def first_code_mod(mods_dir: str) -> str | None:
    """Older tools: the first folder of mods_dir (a mods/ folder or load/mods) with a title folder or exefs in it."""
    if not os.path.isdir(mods_dir):
        return None
    for name in sorted(os.listdir(mods_dir)):
        d = os.path.join(mods_dir, name)
        if not os.path.isdir(d) or name.startswith("."):
            continue
        if re.fullmatch(r"0004000[0eE]00126[13]00", name) or os.path.isdir(os.path.join(d, "exefs")):
            return mods_dir if re.fullmatch(r"0004000[0eE]00126[13]00", name) else d
        try:
            stage_probe = [n for n in os.listdir(d) if re.fullmatch(r"0004000[0eE]00126[13]00", n)]
        except OSError:
            stage_probe = []
        if stage_probe:
            return d
    return None


def install(lib: str, dest_dir: str) -> str:
    """The library and its .id into the player's folder, each written beside its place first and then renamed: a cut
    leaves the old pair or the new one, never half of one."""
    pairs = [(lib, os.path.join(dest_dir, LIBRARY)), (build_game.id_path(lib), os.path.join(dest_dir, build_game.id_path(LIBRARY)))]
    for src, dst in pairs:
        part = dst + ".part"
        shutil.copyfile(src, part)
        with open(part, "rb+") as f:
            os.fsync(f.fileno())
        os.replace(part, dst)
    return pairs[0][1]


def lean_recompile() -> None:
    """The translator's memory, given back as soon as it is no longer read, for a phone's few GB (the same C comes out):
     - recompile() analyses the code twice, the second time with the entry points found through data pointers, and the
       first analysis's functions and the sets made from them (every halfword the code covers) stay alive until it
       returns, through the second analysis and the writing of the C: they go once data_pointer_entries() has used them;
     - build_game.py keeps recompile()'s result, with every function of the second analysis, until it returns, so
       through the whole compile, and reads only the report and the problems from it: the functions go as soon as
       recompile() returns."""
    import gc
    rc = build_game.recompile
    original, cover_of, pointers = rc.recompile, rc.code_cover, rc.data_pointer_entries
    first = []

    def code_cover(funcs):
        first.append(funcs)
        return cover_of(funcs)

    def data_pointer_entries(img, an, segments, known, cover, fall_into, *rest, **kw):
        extra = pointers(img, an, segments, known, cover, fall_into, *rest, **kw)
        for held in (cover, fall_into):
            for s in held.values():
                s.clear()
        known.clear()
        for funcs in first:
            funcs.clear()
        first.clear()
        gc.collect()
        return extra

    def recompile(*a, **k):
        res = original(*a, **k)
        res.pop("funcs", None)
        gc.collect()
        return res
    rc.code_cover, rc.data_pointer_entries, rc.recompile = code_cover, data_pointer_entries, recompile

    # every decoded instruction keeps its C statements as strings, and most are the same few thousand texts
    # ("R(0) = R(1);"): one copy of each (sys.intern), the same strings and so the same C
    from recomp import arm, thumb

    def interned(decode):
        def wrapper(self, *a, **k):
            ins = decode(self, *a, **k)
            if ins.body:
                ins.body = [sys.intern(s) for s in ins.body]
            if ins.pcexpr:
                ins.pcexpr = sys.intern(ins.pcexpr)
            return ins
        return wrapper
    arm.ArmTranslator.decode = interned(arm.ArmTranslator.decode)
    thumb.ThumbTranslator.decode = interned(thumb.ThumbTranslator.decode)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--update", required=True, help="the decrypted update .cia (or the game itself for a version without one)")
    ap.add_argument("--game", help="the base game's decrypted .3ds/.cci: its geometry shaders go into the library")
    ap.add_argument("--mod", help="a code mod to build in (the mods folder or the mod's own folder)")
    ap.add_argument("--game-mods", help="the game folder's mods/: its code mods, as the app copies them")
    ap.add_argument("--user-mods", help="the app's user folder: the mods on in its load/mods, as the game runs them")
    ap.add_argument("--work", required=True, help="the work folder (kept between runs to go on where one stopped)")
    ap.add_argument("--dest", required=True, help="the folder the library and its .id go to (the game's folder)")
    ap.add_argument("--cc", required=True, help="the C compiler (the app's libzigcc.so)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--expect-id", default="", help="the id the app wants: anything else stops before the long part")
    ap.add_argument("--clean", action="store_true", help="once the library is in place, delete the work folder (its C, "
                    "objects and Zig's cache: about 600 MB; a run after that starts from the beginning)")
    args = ap.parse_args(argv)

    t0 = time.time()
    tap = Stages()
    emit(f"@@pid {os.getpid()}")
    lib_id = build_game.code_library_id(True, False)
    emit(f"@@id {lib_id}")
    if args.expect_id and args.expect_id != lib_id:
        emit(f"@@error tools these tools make libraries with id {lib_id}, the app wants {args.expect_id}")
        return 1
    try:
        os.makedirs(args.work, exist_ok=True)
        free = shutil.disk_usage(args.work).free
        out = os.path.join(args.work, "game")
        recomp = os.path.join(out, "recomp")
        have = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(args.work) for f in fs)
        if free + have < WORK_SPACE:
            emit(f"@@error space {(WORK_SPACE - have - free) >> 20} MB more free space needed")
            return 1
        dropped = drop_broken_objects(os.path.join(recomp, "obj"))
        if dropped:
            print(f"{dropped} objects cut short by an earlier run: made again")
        if not os.access(args.dest, os.W_OK):
            emit(f"@@error dest can't write in {args.dest}")
            return 1

        # the stages before the compile: kept from the last run if they were made from the same files and tools
        if args.game_mods:
            mods = f"game-mods {args.game_mods} {mods_digest(args.game_mods)}"
        elif args.user_mods:
            mods = f"user-mods {args.user_mods} {mods_digest(os.path.join(args.user_mods, 'load', 'mods'))}"
        else:
            mods = f"mod {args.mod} {mods_digest(args.mod)}" if args.mod else "mods none"
        lines = [f"tools {lib_id} {tools_digest()}", "python " + " ".join(sys.version.split()),
                 f"update {args.update} {any_digest(args.update)}", f"game {args.game} {any_digest(args.game)}", mods]
        inputs = stamp_hash(lines)
        kept_inputs, kept_made = read_stamp(args.work)
        reuse = kept_inputs == inputs and kept_made != "none" and kept_made == written(out)
        if reuse:
            print(f"inputs as the last run's ({time.time() - t0:.1f} s to check): its translation is kept, on to the compile")
        else:
            drop_stamp(args.work)
            if kept_inputs:
                print("the last run's translation isn't for these inputs (or its C is gone): translating again")

        cmd = ["--dump", args.update, "--native", "--android", "--cc", args.cc, "--out", out, "-j", str(args.jobs)]
        if args.game:
            cmd += ["--romfs", args.game]
        # (build_game.py with --game-mods also reads every mod of a user folder that is on: mh4n feat-4ucode)
        whole = hasattr(build_game, "mod_folders")
        single = args.mod
        if args.game_mods and whole:
            cmd += ["--game-mods", args.game_mods]
            print(f"code mods of {args.game_mods}")
        elif args.user_mods and whole:
            cmd += ["--mods", args.user_mods]
            print(f"code mods that are on in {os.path.join(args.user_mods, 'load', 'mods')}")
        elif args.game_mods:
            single = first_code_mod(args.game_mods)
        elif args.user_mods:
            single = first_code_mod(os.path.join(args.user_mods, "load", "mods"))
        if single and not (whole and (args.game_mods or args.user_mods)):
            if not reuse:  # (the compile alone doesn't read it)
                tap.set("mods")
                title = stage_mod(single, os.path.join(args.work, "mods"))
                print(f"code mod from {title}")
            cmd += ["--mods", os.path.join(args.work, "mods")]
        elif "--game-mods" not in cmd and "--mods" not in cmd:
            cmd += ["--no-mods"]
        tap.set("extract")

        if reuse:
            cmd += ["--compile-only"]
        else:
            lean_recompile()
        progress = CompileProgress(recomp)

        def on_stage(stage):
            if stage != "compile" or progress.is_alive():
                return
            if not reuse:  # (the stages before it are through: what they were made from, and the C they wrote)
                write_stamp(args.work, inputs, written(out), lines)
            print(f"compiling from {time.time() - t0:.1f} s after the start")
            progress.start()
        tap.on_stage = on_stage
        sys.stdout = tap
        try:
            errors = Tail()
            sys.stderr = errors
            try:
                build_game.main(cmd)
            except SystemExit as e:
                # Zig's cache with an object gone that its record still names: a compile of the same C again fails with
                # "cannot open .../zig-cache/o/.../X.o" (a Galaxy S25 Ultra, after some seventy cuts of one build by the
                # heat rule). The cache only saves compiling the same C twice: emptied, the compile goes on once more
                zig_cache = os.path.join(args.work, "zig-cache")
                if e.code in (0, None) or tap.stage != "compile" or "zig-cache" not in errors.text or \
                        not os.path.isdir(zig_cache):
                    raise
                print("Zig's cache names a file it no longer has: emptied, the compile goes on")
                shutil.rmtree(zig_cache, ignore_errors=True)
                build_game.main(cmd if "--compile-only" in cmd else cmd + ["--compile-only"])
        finally:
            progress.stop.set()
            sys.stdout, sys.stderr = sys.__stdout__, sys.__stderr__
        lib = os.path.join(recomp, "android", LIBRARY)
        tap.set("install")
        placed = install(lib, args.dest)
        if args.clean:
            shutil.rmtree(args.work, ignore_errors=True)
        emit(f"@@done {placed}")
        return 0
    except SystemExit as e:
        if e.code in (0, None):
            return 0
        message = str(e.code) if not isinstance(e.code, int) else f"stopped with code {e.code}"
        emit(f"@@error {tap.stage} {' '.join(message.split())}")
        return 1
    except OSError as e:
        traceback.print_exc()
        kind = "space" if e.errno == errno.ENOSPC else tap.stage
        emit(f"@@error {kind} {e}")
        return 1
    except MemoryError:
        emit(f"@@error memory {tap.stage}")
        return 1
    except Exception as e:  # noqa: BLE001 (any other failure: the app shows it, the log has the traceback)
        traceback.print_exc()
        emit(f"@@error {tap.stage} {type(e).__name__}: {' '.join(str(e).split())}")
        return 1


if __name__ == "__main__":
    t0 = time.time()
    code = main()
    print(f"({time.time() - t0:.0f} s)")
    sys.exit(code)
