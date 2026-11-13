package com.mh4u.port;

import android.content.Context;
import android.content.pm.PackageInfo;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Enumeration;
import java.util.Locale;
import java.util.zip.GZIPInputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * The tools the builder (BuilderService) runs, in the app's files under builder/: none of them is in the APK, which stays
 * small, and none of them is game data.
 *  - python/: CPython for Android, the official one from python.org (PSF License), only its library and standard library.
 *  - zig/: Zig 0.13.0 for 64-bit ARM Linux (MIT; its clang and lld are LLVM's, Apache 2.0 with the LLVM exception), from
 *    PyPI's ziglang package: the same Zig the PC tool (mh4n tools/build_game.py --fetch-compiler) downloads on Linux. Only
 *    its program and the parts of its lib folder `zig cc` uses for the game's library (no C libraries: the library is
 *    built with -nostdlib against the few headers of mh4n tools/recomp/runtime/android).
 *  - scripts/: mh4n's tools/ and config/, the very ones the APK was built from (assets/builder/tools.zip, written by
 *    scripts/build_app.sh), so the library they make has the id this app wants.
 * Each download is checked against its SHA-256 before it is unpacked, goes on from where it stopped (a .part file and an
 * HTTP range), and is unpacked into a folder beside its place that is renamed when whole: a cut never leaves half a tool.
 * The same class as MHGU for Android's.
 */
final class BuilderTools {
    /** A download: where from, its SHA-256 and size, and the folder it becomes. */
    static final class Tool {
        final String name, version, url, sha256;
        final long size;
        Tool(String name, String version, String url, String sha256, long size) {
            this.name = name; this.version = version; this.url = url; this.sha256 = sha256; this.size = size;
        }
    }

    static final Tool PYTHON = new Tool("python", "3.14.9",
            "https://www.python.org/ftp/python/3.14.9/python-3.14.9-aarch64-linux-android.tar.gz",
            "e2601ffefb36bd34f49e8743d7ba3dad793bb4478fe879d45773311868b85cc5", 22971453L);
    static final Tool ZIG = new Tool("zig", "0.13.0",
            "https://files.pythonhosted.org/packages/b8/d9/d5e808cbfa20d6b566c27f3445fea30b5ec86d7b45dad237fdc7a8754f53/"
            + "ziglang-0.13.0-py3-none-manylinux_2_17_aarch64.manylinux2014_aarch64.musllinux_1_1_aarch64.whl",
            "5714a4d46f9246ba6180a0930e23df2bc8bc26e62b5fe0dc798b4afca1903308", 77819666L);
    static final Tool[] DOWNLOADS = {PYTHON, ZIG};
    /** What the tools take once unpacked (Zig's program alone is 141 MB). */
    static final long INSTALLED_SIZE = 200L << 20;

    /** How a long step is going, and whether to stop it. */
    interface Progress {
        void step(String what, long done, long total);
        boolean cancelled();
        default void log(String line) {}
    }

    static final class Cancelled extends IOException {
        Cancelled() { super("cancelled"); }
    }

    private BuilderTools() {}

    static File root(Context c) { return new File(c.getFilesDir(), "builder"); }
    static File dir(Context c, Tool t) { return new File(root(c), t.name); }
    static File scripts(Context c) { return new File(root(c), "scripts"); }
    static File python(Context c) { return dir(c, PYTHON); }
    static File zig(Context c) { return new File(dir(c, ZIG), "zig"); }
    static File zigLib(Context c) { return new File(dir(c, ZIG), "lib"); }

    private static File mark(File dir) { return new File(dir, ".installed"); }

    static boolean installed(Context c, Tool t) {
        return (t.name + " " + t.version + " " + t.sha256).equals(readText(mark(dir(c, t))));
    }

    /** The bytes still to download (0 when everything is in place). */
    static long toDownload(Context c) {
        long n = 0;
        for (Tool t : DOWNLOADS)
            if (!installed(c, t)) n += t.size - Math.max(0, Math.min(t.size, part(c, t).length()));
        return n;
    }

    /** The space the tools still need on the phone (the downloads and what they unpack to). */
    static long toInstall(Context c) {
        long n = 0;
        for (Tool t : DOWNLOADS) if (!installed(c, t)) n += t.size + (t == ZIG ? 170L << 20 : 30L << 20);
        return n;
    }

    /** What the tools and the work folder take now. */
    static long used(Context c) { return sizeOf(root(c)); }

    private static File part(Context c, Tool t) { return new File(new File(root(c), "downloads"), t.name + ".part"); }

    /** Everything the builder runs, downloaded and unpacked where it is missing. */
    static void ensure(Context c, Progress p) throws IOException {
        root(c).mkdirs();
        unpackScripts(c);
        for (Tool t : DOWNLOADS) {
            if (installed(c, t)) continue;
            long t0 = System.currentTimeMillis(), had = part(c, t).length();
            File archive = download(c, t, p);
            long t1 = System.currentTimeMillis();
            p.log(t.name + " " + t.version + ": " + ((t.size - had) >> 20) + " MB downloaded in " + (t1 - t0) / 1000 + " s"
                    + (had > 0 ? " (" + (had >> 20) + " MB were already here)" : "") + ", SHA-256 checked");
            File dest = dir(c, t), tmp = new File(root(c), t.name + ".new");
            deleteTree(tmp);
            tmp.mkdirs();
            p.step("unpack:" + t.name, 0, 1);
            if (t == PYTHON) unpackPython(archive, tmp, p);
            else unpackZig(archive, tmp, p);
            writeText(mark(tmp), t.name + " " + t.version + " " + t.sha256);
            deleteTree(dest);
            if (!tmp.renameTo(dest)) throw new IOException("can't move " + tmp + " to " + dest);
            archive.delete();
            p.step("unpack:" + t.name, 1, 1);
            p.log(t.name + ": unpacked in " + (System.currentTimeMillis() - t1) / 1000 + " s, " + (sizeOf(dest) >> 20) + " MB");
        }
    }

    /** The build tools of this APK (assets/builder/tools.zip), again after every install or update of the app. */
    private static void unpackScripts(Context c) throws IOException {
        File dest = scripts(c);
        String stamp;
        try {
            PackageInfo info = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            stamp = info.lastUpdateTime + " " + info.versionName;
        } catch (Exception e) {
            stamp = "?";
        }
        if (stamp.equals(readText(mark(dest)))) return;
        File tmp = new File(root(c), "scripts.new");
        deleteTree(tmp);
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(c.getAssets().open("builder/tools.zip")))) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = zip.getNextEntry()) != null) {
                File out = safe(tmp, e.getName());
                if (e.isDirectory()) { out.mkdirs(); continue; }
                out.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) o.write(buf, 0, n);
                }
            }
        }
        writeText(mark(tmp), stamp);
        deleteTree(dest);
        if (!tmp.renameTo(dest)) throw new IOException("can't move " + tmp + " to " + dest);
    }

    /** The tool's file, whole and checked: from where an earlier try stopped (an HTTP range). */
    private static File download(Context c, Tool t, Progress p) throws IOException {
        File part = part(c, t);
        part.getParentFile().mkdirs();
        if (part.length() > t.size) part.delete();
        for (int attempt = 0; ; attempt++) {
            long have = part.length();
            if (have < t.size) {
                HttpURLConnection http = (HttpURLConnection) new URL(t.url).openConnection();
                http.setConnectTimeout(20000);
                http.setReadTimeout(30000);
                http.setRequestProperty("User-Agent", "mh4u-android-builder");
                if (have > 0) http.setRequestProperty("Range", "bytes=" + have + "-");
                int code = http.getResponseCode();
                boolean append = code == HttpURLConnection.HTTP_PARTIAL;
                if (code != HttpURLConnection.HTTP_OK && !append) {
                    http.disconnect();
                    throw new IOException(t.url + ": HTTP " + code);
                }
                if (!append) have = 0;
                try (InputStream in = http.getInputStream(); OutputStream out = new FileOutputStream(part, append)) {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    long last = 0;
                    while ((n = in.read(buf)) > 0) {
                        if (p.cancelled()) throw new Cancelled();
                        out.write(buf, 0, n);
                        have += n;
                        if (have - last > (256 << 10)) {
                            p.step("download:" + t.name, have, t.size);
                            last = have;
                        }
                        if (have > t.size) break;
                    }
                } finally {
                    http.disconnect();
                }
            }
            p.step("download:" + t.name, Math.min(have, t.size), t.size);
            if (part.length() == t.size && t.sha256.equals(sha256(part))) return part;
            part.delete();  // (another file, or a damaged one: once more from the start, then it's an error)
            if (attempt >= 1) throw new IOException(t.name + ": the download doesn't match its SHA-256 (" + t.url + ")");
        }
    }

    /** From python.org's archive (./prefix/...): the library and the standard library, without its tests and the parts
     *  that need OpenSSL or SQLite (their modules fall back on Python's own: hashlib's built-in hashes). */
    private static void unpackPython(File archive, File dest, Progress p) throws IOException {
        String[] skipDirs = {"test/", "idlelib/", "tkinter/", "turtledemo/", "ensurepip/", "pydoc_data/", "__phello__/",
                             "config-3."};
        String[] skipModules = {"_test", "xx", "_ssl.", "_hashlib.", "_sqlite3.", "_ctypes_test."};
        try (InputStream in = new GZIPInputStream(new BufferedInputStream(new FileInputStream(archive), 1 << 16))) {
            byte[] header = new byte[512];
            byte[] buf = new byte[1 << 16];
            String longName = null;
            while (readFully(in, header)) {
                if (header[0] == 0) break;
                String name = longName != null ? longName : tarName(header);
                longName = null;
                long size = octal(header, 124, 12);
                char type = (char) header[156];
                long padded = (size + 511) & ~511L;
                if (type == 'L' || type == 'x') {  // (a GNU long name, or a pax header with "path=")
                    byte[] data = new byte[(int) size];
                    if (!readFully(in, data)) throw new IOException("python: archive cut short");
                    skip(in, padded - size);
                    String text = new String(data, StandardCharsets.UTF_8);
                    if (type == 'L') longName = text.replace("\0", "");
                    else for (String line : text.split("\n")) {
                        int eq = line.indexOf(" path=");
                        if (eq > 0) longName = line.substring(eq + 6);
                    }
                    continue;
                }
                if (p.cancelled()) throw new Cancelled();
                if (name.startsWith("./")) name = name.substring(2);
                String rel = null;
                if (name.startsWith("prefix/lib/")) {
                    String in1 = name.substring("prefix/lib/".length());
                    if (in1.matches("libpython3\\.\\d+\\.so")) rel = "lib/" + in1;
                    else if (in1.startsWith("python3.")) {
                        String inStd = in1.substring(in1.indexOf('/') + 1);
                        boolean skip = false;
                        for (String d : skipDirs) if (inStd.startsWith(d) || inStd.contains("/" + d)) skip = true;
                        if (inStd.startsWith("lib-dynload/"))
                            for (String m : skipModules) if (inStd.startsWith("lib-dynload/" + m)) skip = true;
                        if (!skip) rel = "lib/" + in1;
                    }
                }
                if (rel != null && (type == '0' || type == '\0')) {
                    File out = safe(dest, rel);
                    out.getParentFile().mkdirs();
                    try (OutputStream o = new FileOutputStream(out)) {
                        long left = size;
                        while (left > 0) {
                            int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                            if (n < 0) throw new IOException("python: archive cut short");
                            o.write(buf, 0, n);
                            left -= n;
                        }
                    }
                    skip(in, padded - size);
                } else {
                    skip(in, padded);
                }
            }
        }
        if (!new File(dest, "lib/python3.14/os.py").isFile() && !hasPython(dest))
            throw new IOException("python: the archive has no standard library");
    }

    private static boolean hasPython(File dest) {
        File[] libs = new File(dest, "lib").listFiles();
        if (libs == null) return false;
        for (File f : libs) if (f.getName().startsWith("python3.") && new File(f, "os.py").isFile()) return true;
        return false;
    }

    /** From the ziglang wheel: ziglang/zig, its licence, and what `zig cc -nostdlib` reads in ziglang/lib. */
    private static void unpackZig(File archive, File dest, Progress p) throws IOException {
        String[] keep = {"ziglang/zig", "ziglang/LICENSE", "ziglang/lib/c.zig", "ziglang/lib/zig.h", "ziglang/lib/compiler_rt.zig",
                         "ziglang/lib/compiler_rt/", "ziglang/lib/compiler/", "ziglang/lib/include/", "ziglang/lib/std/"};
        try (ZipFile zip = new ZipFile(archive)) {
            byte[] buf = new byte[1 << 16];
            int count = 0;
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                String name = e.getName();
                boolean wanted = false;
                for (String k : keep) if (k.endsWith("/") ? name.startsWith(k) : name.equals(k)) wanted = true;
                if (!wanted || e.isDirectory()) continue;
                if (p.cancelled()) throw new Cancelled();
                File out = safe(dest, name.substring("ziglang/".length()));
                out.getParentFile().mkdirs();
                try (InputStream in = zip.getInputStream(e); OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                }
                if (++count % 200 == 0) p.step("unpack:zig", count, zip.size());
            }
        }
        if (!new File(dest, "zig").isFile()) throw new IOException("zig: the archive has no ziglang/zig");
    }

    /** dest/name, refusing a name that would leave dest. */
    private static File safe(File dest, String name) throws IOException {
        File out = new File(dest, name);
        if (!out.getCanonicalPath().startsWith(dest.getCanonicalPath() + File.separator))
            throw new IOException("a path outside the folder: " + name);
        return out;
    }

    private static String tarName(byte[] h) {
        String name = cString(h, 0, 100), prefix = cString(h, 345, 155);
        boolean ustar = h[257] == 'u' && h[258] == 's' && h[259] == 't' && h[260] == 'a' && h[261] == 'r';
        return ustar && !prefix.isEmpty() ? prefix + "/" + name : name;
    }

    private static String cString(byte[] b, int at, int len) {
        int end = at;
        while (end < at + len && b[end] != 0) end++;
        return new String(b, at, end - at, StandardCharsets.UTF_8);
    }

    private static long octal(byte[] b, int at, int len) {
        if ((b[at] & 0x80) != 0) {  // (base-256, for sizes beyond 8 GB: none here, but read it right)
            long v = 0;
            for (int i = 1; i < len; i++) v = v << 8 | (b[at + i] & 0xFF);
            return v;
        }
        long v = 0;
        for (int i = at; i < at + len && b[i] != 0; i++) if (b[i] >= '0' && b[i] <= '7') v = v * 8 + (b[i] - '0');
        return v;
    }

    private static boolean readFully(InputStream in, byte[] b) throws IOException {
        int at = 0;
        while (at < b.length) {
            int n = in.read(b, at, b.length - at);
            if (n < 0) return at == 0 ? false : fail();
            at += n;
        }
        return true;
    }

    private static boolean fail() throws IOException { throw new IOException("archive cut short"); }

    private static void skip(InputStream in, long n) throws IOException {
        while (n > 0) {
            long k = in.skip(n);
            if (k <= 0) {
                if (in.read() < 0) throw new IOException("archive cut short");
                k = 1;
            }
            n -= k;
        }
    }

    static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
            StringBuilder s = new StringBuilder();
            for (byte b : md.digest()) s.append(String.format(Locale.ROOT, "%02x", b));
            return s.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }

    static String readText(File f) {
        try {
            return new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    static void writeText(File f, String s) throws IOException {
        f.getParentFile().mkdirs();
        try (OutputStream o = new FileOutputStream(f)) {
            o.write(s.getBytes(StandardCharsets.UTF_8));
        }
    }

    static long sizeOf(File f) {
        File[] list = f.listFiles();
        if (list == null) return f.length();
        long n = 0;
        for (File k : list) n += sizeOf(k);
        return n;
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File k : list) if (!isLink(k)) deleteTree(k); else k.delete();
        f.delete();
    }

    private static boolean isLink(File f) {
        try {
            return java.nio.file.Files.isSymbolicLink(f.toPath());
        } catch (Exception e) {
            return false;
        }
    }
}
