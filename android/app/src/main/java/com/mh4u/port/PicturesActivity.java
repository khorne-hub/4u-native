package com.mh4u.port;

import android.app.Activity;
import android.app.Dialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * The layout editor's Picture -> Add pictures (app_native/touch.cpp img_tap): pictures of the player's own for the
 * touch buttons, copied as PNG (at most SIDE pixels a side) into the layout profiles' images folder, where the editor
 * lists them: "layout profiles/images" in the save folder the player chose (they go with the profiles), else in the
 * app's folder. PNG, JPG, WebP, BMP or GIF; what the player adds is their own responsibility. Nothing restarts: the editor
 * reads the folder again while its window is open. MHGU for Android has the same screen with the same texts.
 */
public class PicturesActivity extends Activity {
    private static final String TAG = "MH4U";
    private static final int SIDE = 256;

    private boolean es, busy;
    private TextView status;
    private LinearLayout list;
    private Button add;
    private Dialog browser;

    /** The images folder (app_native/touch.cpp profiles_dir + "/images"). */
    static File folder(Activity a) {
        File chosen = SaveFolder.chosen(a);
        File base = chosen != null ? new File(chosen, "layout profiles") : new File(AppHome.home(a), "layout profiles");
        return new File(base, "images");
    }

    private static boolean picture(File f) {
        String n = f.getName().toLowerCase(Locale.ROOT);
        return n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".webp") || n.endsWith(".bmp")
            || n.endsWith(".gif");
    }

    private LinearLayout.LayoutParams wide(int h) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, h);
        lp.setMargins(0, Look.dp(this, 3), 0, Look.dp(this, 3));
        return lp;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        es = Look.es();
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Look.dp(this, 20);
        box.setPadding(pad, Look.dp(this, 14), pad, Look.dp(this, 16));
        box.setBackground(Look.plate(this, Look.WINE, 6));
        TextView title = Look.text(this, es ? "IMÁGENES DE LOS BOTONES" : "BUTTON PICTURES", 20, Look.GOLD, true);
        title.setGravity(Gravity.CENTER);
        box.addView(title);
        TextView body = Look.text(this, es
            ? "Imágenes tuyas para los botones táctiles: PNG, JPG o WebP. Cada una se copia, de " + SIDE + " píxeles por lado como "
                + "mucho, a la carpeta de los perfiles de diseño; después elígela en el editor de diseño con Imagen. Lo que agregues "
                + "es tu responsabilidad."
            : "Pictures of your own for the touch buttons: PNG, JPG or WebP. Each one is copied, at most " + SIDE + " pixels a side, "
                + "into the layout profiles' folder; then pick it in the layout editor with Picture. What you add is your own "
                + "responsibility.", 14, Look.CREAM, false);
        body.setPadding(0, Look.dp(this, 10), 0, Look.dp(this, 6));
        box.addView(body);
        TextView where = Look.text(this, (es ? "Carpeta: " : "Folder: ") + folder(this).getAbsolutePath(), 13, Look.GOLD, false);
        where.setPadding(0, 0, 0, Look.dp(this, 10));
        box.addView(where);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);
        status = Look.text(this, "", 14, Look.GOLD, true);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, Look.dp(this, 8), 0, 0);
        box.addView(status);
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, Look.dp(this, 12), 0, 0);
        add = Look.button(this, es ? "Agregar imágenes" : "Add pictures", true);
        add.setOnClickListener(v -> openBrowser(null));
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(Look.dp(this, 200), Look.dp(this, 46));
        size.setMargins(Look.dp(this, 8), 0, Look.dp(this, 8), 0);
        row.addView(add, size);
        Button close = Look.button(this, es ? "Volver al juego" : "Back to the game", false);
        close.setOnClickListener(v -> { if (!busy) finish(); });
        row.addView(close, new LinearLayout.LayoutParams(size));
        box.addView(row);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Look.PANEL);
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setGravity(Gravity.CENTER);
        page.setPadding(Look.dp(this, 32), Look.dp(this, 10), Look.dp(this, 32), Look.dp(this, 10));
        int room = getResources().getDisplayMetrics().widthPixels - Look.dp(this, 64);
        page.addView(box, new LinearLayout.LayoutParams(Math.min(Look.dp(this, 640), room), LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(page);
        setContentView(scroll);
        refresh();
    }

    private void say(String s, boolean bad) {
        status.setText(s);
        status.setTextColor(bad ? Look.ALERT : Look.GOLD);
    }

    /** The pictures there: each with a small view of it and its Remove. */
    private void refresh() {
        list.removeAllViews();
        File[] all = folder(this).listFiles(f -> f.isFile() && !f.getName().startsWith(".") && picture(f));
        if (all == null || all.length == 0) {
            TextView none = Look.text(this, es ? "(ninguna todavía)" : "(none yet)", 13, Look.CREAM, false);
            none.setGravity(Gravity.CENTER);
            list.addView(none);
            return;
        }
        Arrays.sort(all, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        for (File f : all) {
            LinearLayout r = new LinearLayout(this);
            r.setGravity(Gravity.CENTER_VERTICAL);
            ImageView view = new ImageView(this);
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inSampleSize = 4;
            Bitmap b = BitmapFactory.decodeFile(f.getAbsolutePath(), o);
            if (b != null) view.setImageBitmap(b);
            LinearLayout.LayoutParams vp = new LinearLayout.LayoutParams(Look.dp(this, 40), Look.dp(this, 40));
            vp.setMargins(0, 0, Look.dp(this, 10), 0);
            r.addView(view, vp);
            TextView name = Look.text(this, f.getName(), 14, Look.CREAM, false);
            name.setSingleLine(true);
            r.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            Button remove = Look.button(this, es ? "Quitar" : "Remove", false);
            remove.setOnClickListener(v -> {
                if (busy) return;
                if (!f.delete()) say(es ? "No se pudo quitar." : "It couldn't be removed.", true);
                refresh();
            });
            r.addView(remove, new LinearLayout.LayoutParams(Look.dp(this, 100), Look.dp(this, 42)));
            list.addView(r, wide(LinearLayout.LayoutParams.WRAP_CONTENT));
        }
    }

    // ---- the browser: the storage's folders, and the pictures in them
    private void openBrowser(File at) {
        if (busy) return;
        if (browser != null) browser.dismiss();
        List<File> roots = FolderBrowser.roots(this, false);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(Look.plate(this, Look.PANEL, 8));
        int pad = Look.dp(this, 12);
        box.setPadding(pad, pad, pad, pad);
        TextView head = Look.text(this, es ? "ELIGE UNA IMAGEN" : "CHOOSE A PICTURE", 16, Look.GOLD, true);
        head.setGravity(Gravity.CENTER);
        box.addView(head);
        LinearLayout tabs = new LinearLayout(this);
        tabs.setGravity(Gravity.CENTER);
        File dir = at != null && at.isDirectory() ? at : roots.get(0);
        File root = roots.get(0);
        for (File r : roots) if (dir.getAbsolutePath().startsWith(r.getAbsolutePath())) root = r;
        for (int i = 0; i < roots.size(); i++) {
            File r = roots.get(i);
            Button t = Look.button(this, i == 0 ? (es ? "Memoria interna" : "Phone storage") : (es ? "Tarjeta SD" : "SD card"), r.equals(root));
            t.setOnClickListener(v -> openBrowser(r));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, Look.dp(this, 40));
            lp.setMargins(Look.dp(this, 4), Look.dp(this, 6), Look.dp(this, 4), Look.dp(this, 4));
            tabs.addView(t, lp);
        }
        box.addView(tabs);
        String shown = dir.getAbsolutePath().substring(root.getAbsolutePath().length());
        TextView where = Look.text(this, shown.isEmpty() ? "/" : shown, 13, Look.CREAM, false);
        where.setPadding(Look.dp(this, 4), Look.dp(this, 2), Look.dp(this, 4), Look.dp(this, 6));
        box.addView(where);
        LinearLayout items = new LinearLayout(this);
        items.setOrientation(LinearLayout.VERTICAL);
        final File here = dir;
        if (!dir.equals(root)) {
            Button up = Look.button(this, "⬑  " + (es ? "Subir" : "Up"), false);
            up.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            up.setOnClickListener(v -> openBrowser(here.getParentFile()));
            items.addView(up, wide(Look.dp(this, 42)));
        }
        File[] kids = dir.listFiles(f -> !f.getName().startsWith(".") && (f.isDirectory() || picture(f)));
        if (kids != null) {
            Arrays.sort(kids, (a, b) -> a.isDirectory() != b.isDirectory() ? (a.isDirectory() ? -1 : 1)
                                                                           : a.getName().compareToIgnoreCase(b.getName()));
            for (File k : kids) {
                boolean folder = k.isDirectory();
                Button b = Look.button(this, (folder ? "▸  " : "◆  ") + k.getName(), !folder);
                b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
                b.setOnClickListener(v -> {
                    if (folder) { openBrowser(k); return; }
                    browser.dismiss();
                    start(k);
                });
                items.addView(b, wide(Look.dp(this, 42)));
            }
        }
        if (kids == null || kids.length == 0) {
            TextView none = Look.text(this, es ? "(vacío)" : "(empty)", 13, Look.CREAM, false);
            none.setPadding(Look.dp(this, 6), Look.dp(this, 8), Look.dp(this, 6), Look.dp(this, 8));
            items.addView(none);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(items);
        box.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        Button cancel = Look.button(this, es ? "Cancelar" : "Cancel", false);
        cancel.setOnClickListener(v -> browser.dismiss());
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, Look.dp(this, 8), 0, 0);
        row.addView(cancel, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, Look.dp(this, 44)));
        box.addView(row);
        browser = new Dialog(this);
        browser.requestWindowFeature(Window.FEATURE_NO_TITLE);
        browser.setContentView(box);
        Window w = browser.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            DisplayMetrics m = getResources().getDisplayMetrics();
            w.setLayout(Math.min(Look.dp(this, 640), (int) (m.widthPixels * .92f)), (int) (m.heightPixels * .92f));
        }
        browser.show();
    }

    private static String safeName(String s) {
        String n = s.replaceAll("(?i)\\.(png|jpe?g|webp|bmp|gif)$", "").replaceAll("[^A-Za-z0-9._-]+", "_");
        if (n.length() > 40) n = n.substring(0, 40);
        return (n.isEmpty() || n.startsWith(".") ? "picture" + n : n) + ".png";
    }

    /** The picture decoded, made at most SIDE a side, and written as PNG (a background thread). */
    private void start(File from) {
        busy = true;
        add.setEnabled(false);
        say(es ? "Copiando…" : "Copying…", false);
        new Thread(() -> {
            String done = null, why = null;
            try {
                BitmapFactory.Options o = new BitmapFactory.Options();
                o.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(from.getAbsolutePath(), o);
                int big = Math.max(o.outWidth, o.outHeight);
                if (big <= 0) throw new IOException(es ? "no es una imagen que el teléfono lea" : "it isn't a picture the phone can read");
                o = new BitmapFactory.Options();
                o.inSampleSize = 1;
                while (big / (o.inSampleSize * 2) >= SIDE) o.inSampleSize *= 2;
                Bitmap b = BitmapFactory.decodeFile(from.getAbsolutePath(), o);
                if (b == null) throw new IOException(es ? "no se pudo leer" : "it couldn't be read");
                float k = Math.min(1f, (float) SIDE / Math.max(b.getWidth(), b.getHeight()));
                if (k < 1f) b = Bitmap.createScaledBitmap(b, Math.max(1, Math.round(b.getWidth() * k)), Math.max(1, Math.round(b.getHeight() * k)), true);
                File dir = folder(this);
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException((es ? "no se pudo crear " : "can't make ") + dir.getAbsolutePath());
                String name = safeName(from.getName());
                File to = new File(dir, name), tmp = new File(dir, name + ".part");
                try (OutputStream out = new FileOutputStream(tmp)) {
                    if (!b.compress(Bitmap.CompressFormat.PNG, 100, out)) throw new IOException("PNG");
                }
                to.delete();
                if (!tmp.renameTo(to)) throw new IOException((es ? "no se pudo escribir " : "can't write ") + name);
                done = name;
            } catch (Exception e) {
                Log.e(TAG, "pictures: " + e);
                why = e.getMessage();
            }
            final String ok = done, err = why;
            runOnUiThread(() -> {
                busy = false;
                add.setEnabled(true);
                if (ok != null) say((es ? "Listo: " : "Done: ") + ok + (es ? ". Elígela en el editor con Imagen." : ". Pick it in the editor with Picture."), false);
                else say((es ? "No se pudo: " : "It couldn't be done: ") + err, true);
                refresh();
            });
        }).start();
    }
}
