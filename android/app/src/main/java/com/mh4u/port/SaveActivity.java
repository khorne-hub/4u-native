package com.mh4u.port;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Process;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Settings -> Import save / Export save.
 *
 * MH4U keeps its save as files: system, user1..user3 in the save data (title/00040000/00126300/data/00000001/), and
 * the guild cards, event quests and the Palico in its extra data (extdata/00000000/00001263/). The same layout as
 * Citra and Azahar, and as Checkpoint's and JKSM's backups on a 3DS (the save files alone). Europe's game uses
 * 00126100 and 00001261; the Japanese one (MH4G) 0011d700 and 000011D7.
 *
 * Import: the source is found by a search of the storage, or chosen with the folder browser: an emulator's whole user
 * folder, its sdmc folder or any folder inside it down to the save's own (data, the title folder...), a Checkpoint or
 * JKSM backup (or the folder of its backups: the newest), an Export folder, the files copied by hand (4u-native #16,
 * B096). It is checked (the game it is for, as its place says, against the game the app runs: a save works only in a
 * copy of the same region; the system file; the hunters' files whole), copied into save_import/<the game's title>/,
 * then the app restarts. The start gate (SaveFolderActivity.gate, before the game) applies it (applyPending): a copy of
 * the current save goes to save_backups/<date>, then it is replaced whole (SaveFolder.copySave: staged, then renamed
 * into place), in the save folder when one is chosen, and a notice says what came in or why nothing changed.
 * save_import.txt keeps the last import's lines for Share log.
 * Export: a copy of the save in a folder the player chooses (the system's folder picker, as MHGU's SaveExportActivity):
 * a new folder "MH4U save <date>" in it, in Azahar's layout (sdmc/...), for an emulator or a 3DS.
 */
public class SaveActivity extends Activity {
    static final String EXTRA_EXPORT = "export";
    private static final int PICK_FOLDER = 1;
    private static final int WINE = Color.rgb(38, 18, 14), PANEL = Color.rgb(26, 12, 9), CREAM = Color.rgb(240, 228, 200);
    private static final int GOLD = Color.rgb(226, 182, 92), BRONZE = Color.rgb(62, 38, 22), EDGE = Color.rgb(196, 150, 70);
    private static final int ALERT = Color.rgb(255, 150, 120);
    private static final String ZEROS = "00000000000000000000000000000000";
    private static final String[] SAVE_FILES = {"system", "user1", "user2", "user3"};
    // the save archive's format record (FormatSaveData's values for MH4U, the same in Citra, Azahar and here)
    private static final byte[] METADATA = {0, 0, 4, 0, 0, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0};
    private static final String USA = "00126300", EUR = "00126100", JPN = "0011d700";
    private static final int SYSTEM_SIZE = 512;
    // a hunter's file: the game reads this much of user1..user3; a shorter one shows as "Corrupted Data" in the game's
    // list, a longer one loads (the rest is not read)
    private static final int USER_SIZE = 81408, USER_MAX = 512 * 1024;

    private boolean es, export;
    private TextView status;
    private Button exportButton;
    private LinearLayout list;
    private String game;  // the title of the game the app runs (USA or EUR)

    static File home(Context c) { return AppHome.home(c); }
    static File userDir(Context c) { return new File(home(c), "user"); }
    // (the save is where Settings -> Save folder says: the user folder's sdmc/, or the chosen folder's)
    static File saveDir(Context c, String title) { return SaveFolder.dataDir(SaveFolder.active(c), title); }
    static File extDir(Context c, String title) { return SaveFolder.extDir(SaveFolder.active(c), title); }
    private static File importDir(Context c) { return new File(home(c), "save_import"); }
    private static File recordFile(Context c) { return new File(home(c), "save_import.txt"); }

    /** A line of the last import's record (save_import.txt, for Share log) and of the system log. */
    static void record(Context c, boolean fresh, String line) {
        android.util.Log.i("MH4U", "save import: " + line);
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date());
        try (FileOutputStream out = new FileOutputStream(recordFile(c), !fresh)) {
            out.write((stamp + " " + line + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            android.util.Log.e("MH4U", "save import record: " + e);
        }
    }

    // ---- applying an import before the game starts (SaveFolderActivity.gate; GameActivity.onCreate for a test run)
    /**
     * The save staged in save_import/<title>/{data, extdata} takes the current one's place, which is copied to
     * save_backups first. Nothing is deleted before the new save is copied whole and checked: the staged files are
     * laid out as a save root of their own and SaveFolder.copySave moves them in by renames. A hunter's file brought
     * without its system file takes the current save's (the game needs one there, and any of the same game will do:
     * tested on the PC runner). If anything fails the current save stays as it was. Tried once: the notice says what
     * happened ({title, text}); null when nothing was staged.
     */
    static String[] applyPending(Context c) {
        File staged = importDir(c), ready = new File(staged, "ready");
        if (!ready.exists()) return null;
        boolean es = Look.es();
        File root = SaveFolder.active(c), from = new File(staged, "root");
        String where = AppHome.read(new File(staged, "from.txt"));
        int hunters = 0;
        File backup = null;
        String error = null;
        try {
            SaveFolder.deleteTree(from);
            int titles = 0;
            for (String t : SaveFolder.TITLES) {
                File src = new File(staged, t), data = new File(src, "data");
                if (!data.isDirectory()) continue;
                File system = new File(data, "system");
                if (!system.isFile()) {
                    File mine = new File(SaveFolder.dataDir(root, t), "system");
                    if (!mine.isFile()) throw new IOException(es ? "falta su archivo system" : "its system file is missing");
                    SaveFolder.copyChecked(mine, system);
                    record(c, false, "the current save's system file goes with it");
                }
                titles++;
                for (int u = 1; u <= 3; u++) if (new File(data, "user" + u).length() > 0) hunters++;
                SaveFolder.copyTree(data, SaveFolder.dataDir(from, t));
                File meta = SaveFolder.metaFile(root, t);
                if (meta.isFile()) SaveFolder.copyChecked(meta, SaveFolder.metaFile(from, t));
                else writeBytes(SaveFolder.metaFile(from, t), METADATA);
                if (new File(src, "extdata").isDirectory()) SaveFolder.copyTree(new File(src, "extdata"), SaveFolder.extDir(from, t));
            }
            if (titles == 0) throw new IOException(es ? "no había nada preparado" : "nothing was staged");
            backup = SaveFolder.describe(root).valid ? SaveFolder.backup(c, root, "import") : null;
            SaveFolder.copySave(from, root);
            record(c, false, "applied in " + root + (backup != null ? ", the old save kept in " + backup : " (there was no save before)"));
        } catch (IOException e) {
            error = e.getMessage();
            record(c, false, "NOT applied, the save stays as it was: " + error);
        }
        SaveFolder.deleteTree(from);
        SaveFolder.deleteTree(staged);
        if (error != null)
            return new String[] {es ? "NO SE IMPORTÓ LA PARTIDA" : "THE SAVE WASN'T IMPORTED",
                    (es ? "No se pudo traer la partida: " : "The save couldn't be brought in: ") + error + ".\n\n"
                    + (es ? "Tu partida quedó como estaba. Puedes intentarlo otra vez en Ajustes, Sistema, Tu partida, Importar partida."
                          : "Your save is as it was. You can try again in Settings, System, Your game, Import save.")};
        String who = es ? (hunters == 1 ? "1 cazador" : hunters + " cazadores") : (hunters == 1 ? "1 hunter" : hunters + " hunters");
        String kept = backup == null ? "" : es ? "\n\nLa partida que tenías quedó guardada en save_backups/" + backup.getName() + "."
                                                : "\n\nThe save you had is kept in save_backups/" + backup.getName() + ".";
        return new String[] {es ? "PARTIDA IMPORTADA" : "SAVE IMPORTED",
                (es ? "Se trajo la partida (" + who + ")" : "The save was brought in (" + who + ")")
                + (where.isEmpty() ? "" : (es ? " desde " : " from ") + where) + ". "
                + (es ? "Elige tu cazador en la selección de personaje del juego." : "Pick your hunter on the game's Character Select.") + kept};
    }

    // ---- the screen
    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
    private TextView text(String s, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }
    private GradientDrawable plate(int fill, int r) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setStroke(dp(2), EDGE);
        d.setCornerRadius(dp(r));
        return d;
    }
    private Button button(String s, boolean main) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(main ? GOLD : CREAM);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        if (main) b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setBackground(plate(BRONZE, 6));
        b.setPadding(dp(18), 0, dp(18), 0);
        return b;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        es = "es".equals(Locale.getDefault().getLanguage());
        export = getIntent().getBooleanExtra(EXTRA_EXPORT, false);
        game = SaveFolder.gameTitle(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(plate(PANEL, 8));
        box.setPadding(dp(16), dp(12), dp(16), dp(12));
        TextView title = text(export ? (es ? "EXPORTAR PARTIDA" : "EXPORT SAVE") : (es ? "IMPORTAR PARTIDA" : "IMPORT SAVE"), 19, GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        TextView intro = text(export
                ? (es ? "Elige una carpeta y ahí se crea otra, \"MH4U save\" con la fecha, con la partida (y las tarjetas de gremio, misiones de evento y el Felyne) como la usan Azahar y Citra. Se copia lo último que guardaste en el juego."
                      : "Choose a folder and a new one is made there, \"MH4U save\" with the date, with the save (and the guild cards, event quests and the Palico) as Azahar and Citra use it. It copies what you last saved in the game.")
                : (es ? "Elige de dónde traer la partida. La actual queda guardada en save_backups antes de reemplazarla."
                      : "Choose where to bring the save from. The current one is kept in save_backups before it is replaced."),
                14, CREAM, false);
        intro.setPadding(0, dp(6), 0, dp(8));
        box.addView(intro);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);
        status = text("", 14, GOLD, false);
        status.setPadding(0, dp(8), 0, dp(4));
        box.addView(status);
        Button back = button(es ? "Volver" : "Back", false);
        back.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44));
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        box.addView(back, lp);

        ScrollView scroll = new ScrollView(this);
        LinearLayout page = new LinearLayout(this);
        page.setGravity(Gravity.CENTER);
        page.setBackgroundColor(WINE);
        page.setPadding(dp(32), dp(16), dp(32), dp(16));
        int room = getResources().getDisplayMetrics().widthPixels - dp(64);
        page.addView(box, new LinearLayout.LayoutParams(Math.min(dp(620), room), LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(page);
        scroll.setBackgroundColor(WINE);
        setContentView(scroll);

        if (export) {
            exportButton = button(es ? "Elegir carpeta" : "Choose a folder", true);
            exportButton.setOnClickListener(v -> {
                try {
                    startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
                } catch (android.content.ActivityNotFoundException e) {
                    say(es ? "Este teléfono no tiene selector de carpetas." : "This phone has no folder picker.", true);
                }
            });
            list.addView(exportButton, row());
            if (!hasSave()) {
                exportButton.setEnabled(false);
                say(es ? "Todavía no hay una partida para exportar." : "There is no save to export yet.", true);
            }
        } else {
            // (4u-native #16: a save the search doesn't reach, in any folder the player can point at)
            Button folder = button(es ? "Elegir carpeta" : "Choose a folder", true);
            folder.setOnClickListener(v -> browse());
            list.addView(folder, row());
            status.setText(es ? "Buscando partidas..." : "Looking for saves...");
            new Thread(this::findSources).start();
        }
    }

    // ---- choosing the folder the save is in (the game's folder browser, as MHGU's import has a picker)
    private void browse() {
        if (Build.VERSION.SDK_INT >= 30 ? !Environment.isExternalStorageManager()
                : checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            say(es ? "El juego no tiene permiso para leer tus carpetas: elige otra vez la carpeta del juego en Ajustes para darlo."
                   : "The game has no permission to read your folders: choose the game folder again in Settings to give it.", true);
            return;
        }
        FolderBrowser.open(this, null, es ? "ELIGE LA CARPETA DE TU PARTIDA" : "CHOOSE YOUR SAVE'S FOLDER",
                this::marks,
                (dir, holds) -> holds ? (es ? "✓ Aquí hay una partida." : "✓ There is a save here.")
                                      : (es ? "Entra a la carpeta de tu partida (system, user1...) o a la de tu emulador (sdmc)."
                                            : "Go into your save's folder (system, user1...) or your emulator's (sdmc)."),
                this::chosen, null, false);
    }

    /** The browser's ✓: a quick look (it runs for every folder listed), the places a save is usually kept below dir. */
    private boolean marks(File dir) {
        if (quick(dir) || quick(new File(dir, "00000001")) || quick(new File(dir, "data/00000001"))) return true;
        for (String t : new String[] {USA, EUR, JPN})
            for (String sub : new String[] {t + "/data/00000001", "00040000/" + t + "/data/00000001", "title/00040000/" + t + "/data/00000001"})
                if (quick(new File(dir, sub))) return true;
        File n3ds = dir.getName().equals("Nintendo 3DS") ? dir : new File(dir, "Nintendo 3DS");
        if (!n3ds.isDirectory()) n3ds = new File(dir, "sdmc/Nintendo 3DS");
        if (n3ds.isDirectory() && !emulatorSaves(n3ds).isEmpty()) return true;
        File[] kids = dir.listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));  // (an emulator's folder, a backup's dates)
        if (kids != null && kids.length <= 40)
            for (File k : kids)
                if (new File(k, "sdmc/Nintendo 3DS").isDirectory() || quick(k)) return true;
        // a backup tool's folder of games (3ds/Checkpoint/saves, JKSM/Saves): MH4U's among them
        for (String sub : new String[] {"", "saves", "Saves", "Checkpoint/saves", "3ds/Checkpoint/saves", "JKSM/Saves"}) {
            File games = sub.isEmpty() ? dir : new File(dir, sub);
            String n = games.getName().toLowerCase(Locale.ROOT), up = games.getParentFile() != null ? games.getParentFile().getName() : "";
            if (!n.equals("saves") || !(up.equalsIgnoreCase("Checkpoint") || up.equalsIgnoreCase("JKSM"))) continue;
            File[] each = games.listFiles(File::isDirectory);
            if (each != null)
                for (File g : each) {
                    String gn = g.getName().toLowerCase(Locale.ROOT);
                    if (gn.startsWith("0x01263") || gn.startsWith("0x01261") || gn.contains("monster hunter 4")) return true;
                }
        }
        return false;
    }

    /** A hunter's file in dir, by its usual name (a quick look: no listing). */
    private static boolean quick(File dir) {
        for (int u = 1; u <= 3; u++) if (new File(dir, "user" + u).isFile()) return true;
        return false;
    }

    /** "Use this folder": the saves in it or below it, looked for away from the screen; the best one is brought in. */
    private void chosen(File dir) {
        say(es ? "Buscando la partida en esa carpeta..." : "Looking for the save in that folder...", false);
        new Thread(() -> {
            List<Source> found = new ArrayList<>();
            int[] budget = {4000};
            collect(dir, 0, found, budget);
            Source best = best(found);
            String why = best != null ? null : nothingHere(dir);
            runOnUiThread(() -> {
                if (best == null) {
                    record(this, true, "chosen folder " + dir + ": " + why);
                    refuse(why);
                } else {
                    stage(best);
                }
            });
        }).start();
    }

    // ---- what a folder holds
    /** A save found: the folder of its files, the game its place names (null: it doesn't say), its extra data. */
    private static final class Source {
        File data, ext;
        String title, label, problem;  // problem: why it can't be brought in (null: it can)
        long time;
    }

    /** The save's files in dir by name (system, user1..user3, in any letter case). */
    private static Map<String, File> saveFiles(File dir) {
        Map<String, File> m = new LinkedHashMap<>();
        File[] files = dir.listFiles(File::isFile);
        if (files != null)
            for (File f : files) {
                String n = f.getName().toLowerCase(Locale.ROOT);
                if (Arrays.asList(SAVE_FILES).contains(n) && !m.containsKey(n)) m.put(n, f);
            }
        return m;
    }

    /** How many hunters' files (user1..user3, not empty, not bigger than a save holds) are among them. */
    private static int hunterFiles(Map<String, File> m) {
        int n = 0;
        for (int u = 1; u <= 3; u++) {
            File f = m.get("user" + u);
            if (f != null && f.length() > 0 && f.length() <= USER_MAX) n++;
        }
        return n;
    }

    /** The game a save's place names: its title folder (Azahar, Citra), Checkpoint's backup folder, an extdata id. */
    private static String titleOf(File dir) {
        for (File d = dir; d != null; d = d.getParentFile()) {
            String n = d.getName().toLowerCase(Locale.ROOT);
            if (n.equals(USA) || n.equals("00001263") || n.contains("0004000000126300") || n.startsWith("0x01263")) return USA;
            if (n.equals(EUR) || n.equals("00001261") || n.contains("0004000000126100") || n.startsWith("0x01261")) return EUR;
            if (n.equals(JPN) || n.equals("000011d7") || n.contains("000400000011d700") || n.startsWith("0x011d7")) return JPN;
        }
        return null;
    }

    private static String extId(String title) { return title.equals(EUR) ? "00001261" : title.equals(JPN) ? "000011D7" : "00001263"; }

    /**
     * The extra data beside a save in an emulator's tree: <id1>/title/00040000/<t>/data/00000001 has its guild cards,
     * event quests and Palico in <id1>/extdata/00000000/<its id>/ (user/ in it). null: not that layout, or none there.
     */
    private static File extFor(File data, String title) {
        File d = data.getParentFile(), t = d != null ? d.getParentFile() : null, h = t != null ? t.getParentFile() : null;
        File titles = h != null ? h.getParentFile() : null, id1 = titles != null ? titles.getParentFile() : null;
        if (id1 == null || title == null || !data.getName().equals("00000001") || !d.getName().equals("data")
                || !h.getName().equals("00040000") || !titles.getName().equals("title")) return null;
        for (String id : new String[] {extId(title), extId(title).toLowerCase(Locale.ROOT)}) {
            File ext = new File(id1, "extdata/00000000/" + id);
            if (new File(ext, "user").isDirectory()) return ext;
        }
        return null;
    }

    /** The saves of an emulator's "Nintendo 3DS" folder: <id0>/<id1>/title/00040000/<t>/data/00000001, each game's. */
    private static List<File> emulatorSaves(File n3ds) {
        List<File> out = new ArrayList<>();
        File[] ids0 = n3ds.listFiles(File::isDirectory);
        if (ids0 != null)
            for (File id0 : ids0) {
                File[] ids1 = id0.listFiles(File::isDirectory);
                if (ids1 == null) continue;
                for (File id1 : ids1)
                    for (String t : new String[] {USA, EUR, JPN}) {
                        File d = new File(id1, "title/00040000/" + t + "/data/00000001");
                        if (quick(d)) out.add(d);
                    }
            }
        return out;
    }

    /** dir as a save (null: it holds no hunter's file): where it comes from, and whether and why it can't be used. */
    private Source candidate(File dir) {
        if (!quick(dir)) return null;
        Map<String, File> files = saveFiles(dir);
        if (hunterFiles(files) == 0) return null;
        Source s = new Source();
        s.data = dir;
        s.title = titleOf(dir);
        s.ext = extFor(dir, s.title);
        for (File f : files.values()) s.time = Math.max(s.time, f.lastModified());
        s.label = describe(s, files);
        s.problem = problem(s, files);
        return s;
    }

    private String gameName(String title) {
        return title.equals(EUR) ? (es ? "el europeo" : "the European one")
             : title.equals(JPN) ? (es ? "el japonés (Monster Hunter 4G)" : "the Japanese one (Monster Hunter 4G)")
             : (es ? "el de EE. UU." : "the USA one");
    }

    /** Why a save can't be brought in, in the player's words; null: it can. */
    private String problem(Source s, Map<String, File> files) {
        if (inUse(s.data)) return es ? "Esa es la partida que el juego usa ahora." : "That is the save the game uses now.";
        if (JPN.equals(s.title))
            return es ? "Esa partida es del juego japonés (Monster Hunter 4G): MH4U no puede leerla. No se cambió nada."
                      : "That save is from the Japanese game (Monster Hunter 4G): MH4U can't read it. Nothing was changed.";
        if (s.title != null && !s.title.equals(game))
            return es ? "Esa partida es de un juego de otra región (" + gameName(s.title) + ") y el tuyo es " + gameName(game)
                        + ": una partida solo sirve en un juego de la misma región. No se cambió nada."
                      : "That save is from another region's game (" + gameName(s.title) + ") and yours is " + gameName(game)
                        + ": a save only works in a copy of the same region. Nothing was changed.";
        File system = files.get("system");
        if (system != null && system.length() != SYSTEM_SIZE)
            return es ? "El archivo system de esa carpeta tiene " + system.length() + " bytes y el de MH4U tiene " + SYSTEM_SIZE
                        + ": no es una partida de MH4U, o la copia está dañada. No se cambió nada."
                      : "The system file in that folder is " + system.length() + " bytes and MH4U's is " + SYSTEM_SIZE
                        + ": it isn't an MH4U save, or the copy is damaged. Nothing was changed.";
        if (system == null && !new File(saveDir(this, game), "system").isFile())
            return es ? "En esa carpeta está el archivo del cazador pero no el archivo system que va con él. Cópialo también (está "
                        + "junto a user1 en la carpeta de la partida) y vuelve a importar."
                      : "That folder has the hunter's file but not the system file that goes with it. Copy that one too (it's next "
                        + "to user1 in the save's folder) and import again.";
        for (int u = 1; u <= 3; u++) {
            File f = files.get("user" + u);
            if (f != null && f.length() > 0 && f.length() < USER_SIZE)
                return es ? f.getName() + " está cortado (" + f.length() + " de " + USER_SIZE + " bytes): cópialo otra vez, con el "
                            + "emulador cerrado. No se cambió nada."
                          : f.getName() + " is cut short (" + f.length() + " of " + USER_SIZE + " bytes): copy it again, with the "
                            + "emulator closed. Nothing was changed.";
        }
        return null;
    }

    /** Every save in dir or below it (an emulator's tree, a backup tool's folders...): a walk of at most budget folders. */
    private void collect(File dir, int depth, List<Source> out, int[] budget) {
        if (depth > 9 || --budget[0] < 0 || out.size() > 40) return;
        if (dir.getName().equals("Nintendo 3DS")) {  // (an emulator's: straight to its titles' saves)
            for (File d : emulatorSaves(dir)) {
                Source s = candidate(d);
                if (s != null) out.add(s);
            }
            return;
        }
        Source s = candidate(dir);
        if (s != null) {
            out.add(s);
            return;
        }
        File[] kids = dir.listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (kids == null) return;
        for (File k : kids) {
            if (k.getName().equals("content") || k.getName().equals("cache") || k.getName().equals("shaders")) continue;  // (an emulator's games and caches)
            collect(k, depth + 1, out, budget);
        }
    }

    /** The one to bring in: one that can be (the newest), else the first that can't (to say why). */
    private static Source best(List<Source> found) {
        Source best = null;
        for (Source s : found)
            if (s.problem == null && (best == null || s.time > best.time)) best = s;
        if (best == null && !found.isEmpty()) best = found.get(0);
        return best;
    }

    /** What a folder without a save holds, said (its extra data alone, a raw save image, a .zip), or that it holds none. */
    private String nothingHere(File dir) {
        boolean ext = false, sav = false, zip = false;
        List<File> look = new ArrayList<>();
        look.add(dir);
        for (int i = 0; i < look.size() && i < 400; i++) {
            File d = look.get(i);
            String n = d.getName().toLowerCase(Locale.ROOT);
            if (n.equals("00001263") || n.equals("00001261") || n.equals("000011d7")
                    || new File(d, "cardbox").isFile() || new File(d, "user/cardbox").isFile()) ext = true;
            File[] kids = d.listFiles();
            if (kids == null) continue;
            for (File k : kids) {
                String kn = k.getName().toLowerCase(Locale.ROOT);
                if (k.isDirectory()) { if (!kn.startsWith(".") && look.size() < 400) look.add(k); continue; }
                if (kn.endsWith(".sav")) sav = true;
                if (kn.endsWith(".zip") || kn.endsWith(".7z") || kn.endsWith(".rar")) zip = true;
            }
        }
        if (sav)
            return es ? "Ahí hay una imagen de partida (.sav, de la tarjeta SD de una consola o de un volcado del cartucho): el juego no "
                        + "puede leerla así. Respáldala con Checkpoint o JKSM en la consola e importa esa carpeta."
                      : "There's a raw save image there (.sav, from a console's SD card or a cartridge dump): the game can't read it "
                        + "like that. Back the save up with Checkpoint or JKSM on the console and import that folder.";
        if (ext)
            return es ? "Ahí están los datos extra (tarjetas de gremio, misiones de evento, Felyne), pero no la partida. La partida está "
                        + "en title/00040000/00126300/data/00000001 (Azahar, Citra) o en la copia \"saves\" de Checkpoint. Las misiones "
                        + "de evento se traen con Importar DLC y misiones de evento."
                      : "That holds the extra data (guild cards, event quests, Palico), not the save. The save is in "
                        + "title/00040000/00126300/data/00000001 (Azahar, Citra) or in Checkpoint's \"saves\" backup. Event quests "
                        + "come in with Import DLC and event quests.";
        String base = es ? "En esa carpeta no hay una partida de MH4U. Elige la que tiene system y user1, o la carpeta sdmc de tu emulador."
                         : "There is no MH4U save in that folder. Choose the one with system and user1, or your emulator's sdmc folder.";
        return zip ? base + (es ? " Si tu partida está en un .zip, descomprímelo primero." : " If your save is in a .zip, unzip it first.") : base;
    }

    private LinearLayout.LayoutParams row() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(46));
        lp.setMargins(0, dp(3), 0, dp(3));
        return lp;
    }

    /** A found save's row: as tall as its label (a long one takes two lines). */
    private LinearLayout.LayoutParams rowWrap() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(3), 0, dp(3));
        return lp;
    }

    // ---- the search of the storage
    private void findSources() {
        List<Source> found = new ArrayList<>();
        File root = Environment.getExternalStorageDirectory();
        List<File> starts = new ArrayList<>();
        starts.add(root);
        File[] mine = getExternalFilesDirs(null);
        if (mine != null)
            for (File f : mine) {
                if (f == null) continue;
                String p = f.getAbsolutePath();
                int cut = p.indexOf("/Android/data/");
                if (cut > 0 && !new File(p.substring(0, cut)).equals(root)) starts.add(new File(p.substring(0, cut)));
            }
        for (File s : starts) scan(s, 0, found);
        runOnUiThread(() -> show(found));
    }

    private void scan(File dir, int depth, List<Source> out) {
        if (depth > 6 || out.size() > 20) return;
        if (dir.getName().equals("Nintendo 3DS")) {  // (an emulator's: its saves are 6 levels further, past the search's depth)
            for (File d : emulatorSaves(dir)) {
                Source s = candidate(d);
                if (s != null && !inUse(s.data)) out.add(s);
            }
            return;
        }
        if (depth > 0) {
            Source s = candidate(dir);  // (a Checkpoint or JKSM backup, a save's own folder, the files copied by hand)
            if (s != null) {
                if (!inUse(s.data)) out.add(s);
                return;
            }
        }
        File[] kids = dir.listFiles(f -> f.isDirectory() && !f.getName().startsWith("."));
        if (kids == null) return;
        for (File k : kids) {
            if (depth == 0 && k.getName().equals("Android")) {
                File data = new File(k, "data");  // (other apps' folders: Android 11 and later keep them closed)
                File[] apps = data.listFiles(File::isDirectory);
                if (apps != null) for (File a : apps) if (!a.getName().equals(getPackageName())) scan(new File(a, "files"), 2, out);
                continue;
            }
            if (k.getName().equals("content")) continue;
            scan(k, depth + 1, out);
        }
    }

    /** A save the game itself uses (its own folder, or the save folder chosen): not something to import. */
    private boolean inUse(File dir) {
        String p = dir.getAbsolutePath();
        return p.startsWith(userDir(this).getAbsolutePath() + "/") || p.startsWith(SaveFolder.active(this).getAbsolutePath() + "/");
    }

    /** "Azahar · European game · 1 hunter · 2026-11-06 14:45": where it is, as the player would know it. */
    private String describe(Source s, Map<String, File> files) {
        String p = s.data.getAbsolutePath(), low = p.toLowerCase(Locale.ROOT);
        String where = low.contains("citra") ? "Citra" : low.contains("azahar") ? "Azahar" : low.contains("lime3ds") ? "Lime3DS"
                     : low.contains("/checkpoint/") ? "Checkpoint" : low.contains("/jksm/") ? "JKSM" : null;
        if (where == null) {  // (the folder the player would know it by: the one that holds sdmc, else the save's own)
            String root = Environment.getExternalStorageDirectory().getAbsolutePath();
            int cut = p.indexOf("/sdmc/");
            String place = cut > 0 ? p.substring(0, cut) : s.data.getName().equals("00000001") ? s.data.getParent() : p;
            where = place.startsWith(root + "/") ? place.substring(root.length() + 1) : new File(place).getName();
        }
        int users = hunterFiles(files);
        String region = s.title == null || s.title.equals(game) ? ""
                : " · " + (s.title.equals(EUR) ? (es ? "juego europeo" : "European game")
                         : s.title.equals(JPN) ? (es ? "juego japonés" : "Japanese game") : (es ? "juego de EE. UU." : "USA game"));
        String date = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(new Date(s.time));
        return where + region + " · " + users + (es ? (users == 1 ? " personaje" : " personajes") : (users == 1 ? " hunter" : " hunters")) + " · " + date;
    }

    private void show(List<Source> found) {
        status.setText(found.isEmpty()
                ? (es ? "La búsqueda no encontró ninguna partida de MH4U. Usa Elegir carpeta y entra a la carpeta de tu partida (system, user1...) o a la de tu emulador. Una partida dentro de Android/data de otra app hay que copiarla afuera primero."
                      : "The search found no MH4U save. Use Choose a folder and go to your save's folder (system, user1...) or your emulator's. A save inside another app's Android/data has to be copied out first.")
                : (es ? "Toca la partida que quieres traer, o elige su carpeta." : "Tap the save to bring in, or choose its folder."));
        for (Source s : found) {
            Button b = button("▸  " + s.label, s.problem == null);
            b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            b.setMinHeight(dp(46));
            b.setPadding(dp(18), dp(6), dp(18), dp(6));
            b.setOnClickListener(v -> stage(s));
            list.addView(b, rowWrap());
        }
    }

    /** The save into save_import/<the game's title>/ (checked first), then the app starts again and the gate applies it. */
    private void stage(Source s) {
        record(this, true, "from " + s.data + " (" + s.label + "), the place says " + (s.title == null ? "nothing of the game" : s.title)
                + ", the game is " + game + (s.ext != null ? ", extdata " + s.ext : ", no extdata"));
        if (s.problem != null) {
            record(this, false, "refused: " + s.problem);
            refuse(s.problem);
            return;
        }
        File staged = importDir(this);
        deleteTree(staged);
        File to = new File(staged, game);
        File data = new File(to, "data");
        data.mkdirs();
        StringBuilder names = new StringBuilder();
        try {
            for (Map.Entry<String, File> f : saveFiles(s.data).entrySet()) {
                if (!f.getKey().equals("system") && f.getValue().length() == 0) continue;  // (an empty slot)
                SaveFolder.copyChecked(f.getValue(), new File(data, f.getKey()));
                names.append(' ').append(f.getKey()).append('=').append(f.getValue().length());
            }
            if (s.ext != null) SaveFolder.copyTree(s.ext, new File(to, "extdata"));
        } catch (IOException e) {
            deleteTree(staged);
            record(this, false, "not copied: " + e.getMessage());
            say(es ? "No se pudo copiar esa partida. Cópiala a la memoria del teléfono y vuelve a intentarlo."
                   : "That save could not be copied. Copy it to the phone's storage and try again.", true);
            return;
        }
        String where = s.label.substring(0, s.label.indexOf(" · ") > 0 ? s.label.indexOf(" · ") : s.label.length());
        AppHome.writeText(new File(staged, "from.txt"), where);
        writeBytes(new File(staged, "ready"), new byte[0]);
        record(this, false, "staged:" + names + (s.ext != null ? " + extdata" : ""));
        status.setTextColor(GOLD);
        status.setText(es ? "Listo. El juego se reinicia y carga la partida." : "Done. The game restarts and loads the save.");
        list.removeAllViews();
        // the game may hold the old save in memory and write it at any moment: it is replaced before it starts again.
        // PhoenixActivity ends this process and opens the app at once, so the app's entry is what Android brings back (a
        // process that ended by itself first had the game's screen brought back without the entry, and a second restart)
        Intent intent = new Intent(this, PhoenixActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(PhoenixActivity.EXTRA_PID, Process.myPid());
        startActivity(intent);
    }

    /** Why a save can't come in: under the list and in a notice (a long list can hide the line under it). */
    private void refuse(String why) {
        say(why, true);
        if (!isFinishing())
            Look.notice(this, es ? "NO SE PUEDE IMPORTAR" : "IT CAN'T BE IMPORTED", why,
                    new Look.Choice(es ? "Entendido" : "OK", true, null));
    }

    private void say(String text, boolean bad) {
        status.setTextColor(bad ? ALERT : GOLD);
        status.setText(text);
    }

    private boolean hasSave() {
        for (String t : new String[] {"00126300", "00126100"})
            if (new File(saveDir(this, t), "system").exists()) return true;
        return false;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        exportButton.setEnabled(false);
        say(es ? "Copiando..." : "Copying...", false);
        new Thread(() -> {
            String folder = null, error = null;
            try {
                folder = doExport(tree);
            } catch (IOException | RuntimeException e) {
                android.util.Log.e("MH4U", "save export: " + e);
                error = e.getMessage();
            }
            final String done = folder, why = error;
            runOnUiThread(() -> {
                exportButton.setEnabled(true);
                if (done != null)
                    say(es ? "Listo: carpeta \"" + done + "\". Copia su carpeta sdmc sobre la de Azahar o Citra."
                           : "Done: folder \"" + done + "\". Copy its sdmc folder over Azahar's or Citra's.", false);
                else
                    say((es ? "No se pudo copiar: " : "Could not copy it: ") + why, true);
            });
        }).start();
    }

    /** A new folder in the one chosen, with the save in Azahar's layout; the files' sizes checked. Its name. */
    private String doExport(Uri tree) throws IOException {
        ContentResolver cr = getContentResolver();
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String name = "MH4U save " + new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.ROOT).format(new Date());
        Uri out = DocumentsContract.createDocument(cr, parent, DocumentsContract.Document.MIME_TYPE_DIR, name);
        if (out == null) throw new IOException(es ? "esa carpeta no admite archivos nuevos" : "that folder takes no new files");
        int copied = 0;
        for (String t : new String[] {"00126300", "00126100"}) {
            File save = saveDir(this, t);
            if (!new File(save, "system").exists()) continue;
            Uri base = dirs(cr, out, "sdmc", "Nintendo 3DS", ZEROS, ZEROS);
            Uri title = dirs(cr, base, "title", "00040000", t, "data");
            copyOut(cr, save, title);  // (data/00000001/, the save's own folder name)
            File meta = new File(save.getParentFile(), "00000001.metadata");
            if (meta.exists()) copyOut(cr, meta, title);
            File ext = extDir(this, t);
            if (ext.isDirectory()) copyOut(cr, ext, dirs(cr, base, "extdata", "00000000"));
            copied++;
        }
        if (copied == 0) throw new IOException(es ? "todavía no hay una partida" : "there is no save yet");
        android.util.Log.i("MH4U", "save export: " + name);
        return name;
    }

    /** The folders `names` under `at`, made one inside the other (a folder already there is used). */
    private static Uri dirs(ContentResolver cr, Uri at, String... names) throws IOException {
        for (String n : names) at = child(cr, at, n, true);
        return at;
    }

    /** The child called `name` of the folder `dir`, made if it isn't there (a folder, or an empty file). */
    private static Uri child(ContentResolver cr, Uri dir, String name, boolean folder) throws IOException {
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(dir, DocumentsContract.getDocumentId(dir));
        try (android.database.Cursor c = cr.query(kids, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (c != null && c.moveToNext())
                if (name.equals(c.getString(1))) return DocumentsContract.buildDocumentUriUsingTree(dir, c.getString(0));
        }
        Uri made = DocumentsContract.createDocument(cr, dir, folder ? DocumentsContract.Document.MIME_TYPE_DIR : "application/octet-stream", name);
        if (made == null) throw new IOException(name);
        return made;
    }

    /** A file or a folder of the app's into the folder `to` (a document folder), under its own name. */
    private static void copyOut(ContentResolver cr, File from, Uri to) throws IOException {
        if (from.isDirectory()) {
            Uri dir = child(cr, to, from.getName(), true);
            File[] list = from.listFiles();
            if (list != null) for (File f : list) copyOut(cr, f, dir);
            return;
        }
        Uri file = child(cr, to, from.getName(), false);
        long copied = 0;
        try (FileInputStream in = new FileInputStream(from); OutputStream out = cr.openOutputStream(file, "wt")) {
            if (out == null) throw new IOException(from.getName());
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; copied += n) out.write(buf, 0, n);
        }
        if (copied != from.length()) throw new IOException(from.getName());
    }

    // ---- files
    static void copyTree(File from, File to) {
        if (!from.exists()) return;
        if (from.isDirectory()) {
            to.mkdirs();
            File[] list = from.listFiles();
            if (list != null) for (File f : list) copyTree(f, new File(to, f.getName()));
            return;
        }
        to.getParentFile().mkdirs();
        try (FileInputStream in = new FileInputStream(from); FileOutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (IOException e) {
            android.util.Log.e("MH4U", "copy " + from + ": " + e);
        }
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File k : list) deleteTree(k);
        f.delete();
    }

    static void writeBytes(File f, byte[] bytes) {
        f.getParentFile().mkdirs();
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(bytes);
        } catch (IOException e) {
            android.util.Log.e("MH4U", "write " + f + ": " + e);
        }
    }
}
