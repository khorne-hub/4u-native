package com.mh4u.port;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputFilter;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;

/**
 * Settings -> Shortcut with your image (S56): a shortcut to the game on the home screen, with a picture the player picks
 * and the name they want. MHGU for Android has the same screen, name and texts.
 *
 * Android lets an app change its own icon only to pictures it ships (activity aliases), so a player's own picture goes
 * on a pinned shortcut (ShortcutManager.requestPinShortcut, Android 8 and later; the home screen app asks to place it).
 * It opens the app as its icon does (the launcher's activity, MAIN / LAUNCHER: a game already running comes back as it
 * was). The picture is an adaptive icon: a square of 108 dp of which the home screen shows about the middle 72 under its
 * shape, so the picture fills that middle part, its own scaled-up copy behind it to the edges; the preview here shows
 * that middle part. The picture is kept in the app's private files (shortcut.png) to show it again here. One shortcut of
 * the app's own (SHORTCUT_ID): doing it again with another picture updates the one placed.
 */
public class ShortcutActivity extends Activity {
    private static final String TAG = "MH4U";
    private static final String SHORTCUT_ID = "own-image";
    private static final int PICK = 1;
    private static final int ICON = 432, SHOWN = 304;  // (108 dp at 4x; the middle 72 dp and a little more)

    private boolean es;
    private TextView status;
    private ImageView preview;
    private EditText name;
    private Button add;
    private Bitmap icon;  // (null: no picture chosen yet)

    private File kept() { return new File(getFilesDir(), "shortcut.png"); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        es = Look.es();
        int pad = Look.dp(this, 14);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad * 2, Look.dp(this, 8), pad * 2, Look.dp(this, 6));

        TextView title = Look.text(this, es ? "ACCESO DIRECTO CON TU IMAGEN" : "SHORTCUT WITH YOUR IMAGE", 20, Look.GOLD, true);
        title.setGravity(Gravity.CENTER);
        page.addView(title);
        TextView intro = Look.text(this, es ? "Un acceso directo al juego en tu pantalla de inicio, con la imagen y el nombre que quieras."
                                            : "A shortcut to the game on your home screen, with the picture and the name you want.", 14, Look.CREAM, false);
        intro.setGravity(Gravity.CENTER);
        intro.setPadding(0, Look.dp(this, 2), 0, Look.dp(this, 10));
        page.addView(intro);

        LinearLayout middle = new LinearLayout(this);
        middle.setOrientation(LinearLayout.HORIZONTAL);
        middle.setGravity(Gravity.CENTER_VERTICAL);
        preview = new ImageView(this);
        preview.setBackground(Look.plate(this, Look.PANEL, 22));
        int side = Look.dp(this, 112);
        LinearLayout.LayoutParams pv = new LinearLayout.LayoutParams(side, side);
        pv.setMargins(0, 0, Look.dp(this, 18), 0);
        middle.addView(preview, pv);

        LinearLayout right = new LinearLayout(this);
        right.setOrientation(LinearLayout.VERTICAL);
        right.addView(Look.text(this, es ? "NOMBRE" : "NAME", 13, Look.GOLD, true));
        name = new EditText(this);
        name.setSingleLine(true);
        name.setFilters(new InputFilter[] {new InputFilter.LengthFilter(24)});
        name.setText(getApplicationInfo().loadLabel(getPackageManager()).toString());
        name.setTextColor(Look.CREAM);
        name.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        name.setBackground(Look.plate(this, Look.PANEL, 6));
        name.setPadding(Look.dp(this, 10), Look.dp(this, 6), Look.dp(this, 10), Look.dp(this, 6));
        right.addView(name, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView how = Look.text(this, es
                ? "• Tu pantalla de inicio te pide confirmar dónde ponerlo.\n"
                  + "• El ícono propio de la app no cambia. Hacerlo otra vez cambia la imagen del acceso directo.\n"
                  + "• La imagen queda en tu teléfono. Usa una que tengas derecho a usar."
                : "• Your home screen asks you to confirm where it goes.\n"
                  + "• The app's own icon doesn't change. Doing it again changes the shortcut's picture.\n"
                  + "• The picture stays on your phone. Use one you have the right to use.", 13, Look.CREAM, false);
        how.setPadding(0, Look.dp(this, 8), 0, 0);
        how.setLineSpacing(Look.dp(this, 1), 1);
        right.addView(how);
        middle.addView(right, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        page.addView(middle);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        LinearLayout whole = new LinearLayout(this);
        whole.setOrientation(LinearLayout.VERTICAL);
        whole.setBackgroundColor(Look.WINE);
        whole.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        status = Look.text(this, "", 14, Look.GOLD, false);
        status.setGravity(Gravity.CENTER);
        status.setPadding(pad * 2, Look.dp(this, 4), pad * 2, Look.dp(this, 4));
        status.setVisibility(View.GONE);
        whole.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER);
        row.setPadding(0, 0, 0, Look.dp(this, 10));
        Button pick = Look.button(this, es ? "Elegir imagen" : "Choose a picture", true);
        pick.setOnClickListener(v -> openPicker());
        add = Look.button(this, es ? "Poner en el inicio" : "Add to home screen", true);
        add.setOnClickListener(v -> place());
        Button leave = Look.button(this, es ? "Volver" : "Back", false);
        leave.setOnClickListener(v -> finish());
        for (Button b : new Button[] {pick, add, leave}) {
            LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, Look.dp(this, 46));
            size.setMargins(Look.dp(this, 8), 0, Look.dp(this, 8), 0);
            row.addView(b, size);
        }
        whole.addView(row);
        whole.setOnApplyWindowInsetsListener((v, insets) -> {  // (the buttons stay above the system's bar)
            v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(), insets.getSystemWindowInsetRight(),
                    insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(whole);

        Bitmap before = kept().isFile() ? BitmapFactory.decodeFile(kept().getAbsolutePath()) : null;
        if (before != null && before.getWidth() == ICON && before.getHeight() == ICON) {
            icon = before;
            preview.setImageBitmap(shown(icon));
        }
        add.setEnabled(icon != null);
        add.setAlpha(icon != null ? 1f : .5f);
        pick.requestFocus();  // (a controller: A presses it)
        ShortcutManager sm = getSystemService(ShortcutManager.class);
        if (sm == null || !sm.isRequestPinShortcutSupported())
            say(es ? "Tu pantalla de inicio no acepta accesos directos de apps." : "Your home screen doesn't accept shortcuts from apps.", true);
    }

    private void say(String s, boolean bad) {
        status.setTextColor(bad ? Look.ALERT : Look.GOLD);
        status.setText(s);
        status.setVisibility(View.VISIBLE);
    }

    private void openPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("image/*");
        try {
            startActivityForResult(intent, PICK);
        } catch (ActivityNotFoundException e) {
            say(es ? "Este teléfono no tiene selector de archivos." : "This phone has no file picker.", true);
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        Bitmap b = load(data.getData());
        if (b == null) {
            say(es ? "No se pudo abrir esa imagen. Prueba con otra (PNG o JPG)." : "That picture could not be opened. Try another one (PNG or JPG).", true);
            return;
        }
        icon = b;
        preview.setImageBitmap(shown(icon));
        add.setEnabled(true);
        add.setAlpha(1f);
        add.requestFocus();
        say(es ? "Así se va a ver. Elige el nombre y toca Poner en el inicio." : "This is how it will look. Pick the name and tap Add to home screen.", false);
    }

    /** The picture chosen as the icon's square: read at about the size needed, its middle square, ICON pixels a side. */
    private Bitmap load(Uri uri) {
        try {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            try (InputStream in = getContentResolver().openInputStream(uri)) { BitmapFactory.decodeStream(in, null, o); }
            if (o.outWidth <= 0 || o.outHeight <= 0) return null;
            int sample = 1;
            while (Math.min(o.outWidth, o.outHeight) / (sample * 2) >= ICON) sample *= 2;
            BitmapFactory.Options read = new BitmapFactory.Options();
            read.inSampleSize = sample;
            Bitmap src;
            try (InputStream in = getContentResolver().openInputStream(uri)) { src = BitmapFactory.decodeStream(in, null, read); }
            if (src == null) return null;
            int s = Math.min(src.getWidth(), src.getHeight());
            Rect square = new Rect((src.getWidth() - s) / 2, (src.getHeight() - s) / 2, (src.getWidth() + s) / 2, (src.getHeight() + s) / 2);
            Bitmap out = Bitmap.createBitmap(ICON, ICON, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(out);
            Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
            c.drawBitmap(src, square, new Rect(0, 0, ICON, ICON), p);  // (behind: to the edges, under the home screen's shape)
            int m = (ICON - SHOWN) / 2;
            c.drawBitmap(src, square, new Rect(m, m, ICON - m, ICON - m), p);  // (the part the home screen shows)
            src.recycle();
            return out;
        } catch (IOException | SecurityException | OutOfMemoryError e) {
            Log.e(TAG, "shortcut: " + e);
            return null;
        }
    }

    /** The preview: the middle of the icon that the home screen shows, in a rounded square. */
    private Bitmap shown(Bitmap full) {
        int m = (ICON - 288) / 2, s = 288;
        Bitmap out = Bitmap.createBitmap(s, s, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(out);
        Path round = new Path();
        round.addRoundRect(new RectF(0, 0, s, s), s * .22f, s * .22f, Path.Direction.CW);
        c.clipPath(round);
        c.drawBitmap(full, new Rect(m, m, m + s, m + s), new Rect(0, 0, s, s), new Paint(Paint.FILTER_BITMAP_FLAG));
        return out;
    }

    private void place() {
        if (icon == null) return;
        ShortcutManager sm = getSystemService(ShortcutManager.class);
        if (sm == null || !sm.isRequestPinShortcutSupported()) {
            say(es ? "Tu pantalla de inicio no acepta accesos directos de apps." : "Your home screen doesn't accept shortcuts from apps.", true);
            return;
        }
        String label = name.getText().toString().trim();
        if (label.isEmpty()) label = getApplicationInfo().loadLabel(getPackageManager()).toString();
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        ComponentName entry = launch != null ? launch.getComponent() : null;
        if (entry == null) {
            say(es ? "No se encontró la pantalla de inicio de la app." : "The app's start screen was not found.", true);
            return;
        }
        // (as the app's icon opens it: a game already running comes back as it was)
        Intent open = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER).setComponent(entry)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        ShortcutInfo info = new ShortcutInfo.Builder(this, SHORTCUT_ID)
                .setShortLabel(label)
                .setLongLabel(label)
                .setIcon(Icon.createWithAdaptiveBitmap(icon))
                .setIntent(open)
                .build();
        try (FileOutputStream out = new FileOutputStream(kept())) {
            icon.compress(Bitmap.CompressFormat.PNG, 100, out);
        } catch (IOException e) {
            Log.w(TAG, "shortcut: the picture was not kept: " + e);
        }
        try {
            boolean placed = false;
            for (ShortcutInfo s : sm.getPinnedShortcuts()) placed |= SHORTCUT_ID.equals(s.getId());
            if (placed) {
                sm.updateShortcuts(Collections.singletonList(info));
                say(es ? "Listo: el acceso directo de tu pantalla de inicio ya tiene esta imagen y este nombre."
                       : "Done: the shortcut on your home screen now has this picture and name.", false);
            } else if (sm.requestPinShortcut(info, null)) {
                say(es ? "Tu pantalla de inicio te pide confirmar dónde ponerlo." : "Your home screen asks you to confirm where it goes.", false);
            } else {
                say(es ? "Tu pantalla de inicio no lo aceptó." : "Your home screen didn't accept it.", true);
            }
            Log.i(TAG, "shortcut: " + (placed ? "updated" : "asked to place") + " \"" + label + "\"");
        } catch (IllegalArgumentException | IllegalStateException e) {  // (the app in the background, a rate limit)
            Log.e(TAG, "shortcut: " + e);
            say(es ? "No se pudo ahora. Vuelve a intentarlo en un momento." : "It couldn't be done now. Try again in a moment.", true);
        }
    }
}
