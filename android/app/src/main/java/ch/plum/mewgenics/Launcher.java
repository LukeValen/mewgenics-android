package ch.plum.mewgenics;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

/** Start screen: where the game data is, importing it, and starting the game. */
public class Launcher extends Activity {
    private static final int PICK_FOLDER = 1;
    static final String PREFS = "mewgenics";
    static final String PREF_FULL_RES = "full_res";
    private TextView status;
    private ProgressBar bar;
    private Button play, importBtn;
    private GameImporter importer;
    private final Handler ui = new Handler(Looper.getMainLooper());

    /** Where the game data is imported to (Launcher import, USB copy). */
    static File importDir(Activity a) { return new File(a.getExternalFilesDir(null), "game"); }
    /** Where Plum Hub's store puts the app's companion files (its OBB folder). */
    static File obbDir(Activity a) { return a.getObbDir(); }
    /** The game data the game runs from: the imported copy, else the Plum Hub download. */
    static File gameDir(Activity a) {
        File imp = importDir(a);
        if (GameImporter.installed(imp)) return imp;
        File obb = obbDir(a);
        if (obb != null && GameImporter.installed(obb)) return obb;
        return imp;
    }
    static File userDir(Activity a) { return new File(a.getExternalFilesDir(null), "user"); }

    private int dp(int v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        // Relaunched from the home screen while the game runs further up the same task: show the game.
        if (!isTaskRoot() && Intent.ACTION_MAIN.equals(getIntent().getAction())
                && getIntent().hasCategory(Intent.CATEGORY_LAUNCHER)) {
            finish();
            return;
        }
        importDir(this).mkdirs();
        userDir(this).mkdirs();
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(32), dp(28), dp(32), dp(28));
        col.setBackgroundColor(Color.rgb(24, 18, 30));

        TextView title = new TextView(this);
        title.setText("MEWGENICS");
        title.setTextColor(Color.rgb(255, 214, 120));
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 34);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        col.addView(title);
        TextView sub = new TextView(this);
        sub.setText("Statische Rekompilierung für Android");
        sub.setTextColor(Color.rgb(190, 180, 200));
        sub.setPadding(0, 0, 0, dp(20));
        col.addView(sub);

        status = new TextView(this);
        status.setTextColor(Color.rgb(230, 230, 235));
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        status.setPadding(0, 0, 0, dp(12));
        col.addView(status);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        col.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(14)));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(16), 0, 0);
        play = new Button(this);
        play.setText("Spielen");
        play.setOnClickListener(v -> startGame());
        importBtn = new Button(this);
        importBtn.setText("Spieldaten importieren (Ordner wählen)");
        importBtn.setOnClickListener(v -> pickFolder());
        row.addView(play);
        row.addView(importBtn);
        col.addView(row);

        CheckBox fullRes = new CheckBox(this);
        fullRes.setText("Volle Bildschirmauflösung (schärfer, aber langsamer; sonst max. " + MewActivity.RENDER_MAX + " px Breite)");
        fullRes.setTextColor(Color.rgb(230, 230, 235));
        fullRes.setPadding(dp(4), dp(12), 0, 0);
        fullRes.setChecked(getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_FULL_RES, false));
        fullRes.setOnCheckedChangeListener((v, on) ->
                getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_FULL_RES, on).apply());
        col.addView(fullRes);

        TextView help = new TextView(this);
        help.setTextColor(Color.rgb(160, 150, 170));
        help.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        help.setPadding(0, dp(20), 0, 0);
        help.setText("Benötigt werden Mewgenics.exe und resources.gpak (etwa 5,1 GB) aus deiner eigenen Steam-Installation. "
                + "Entweder über „Spieldaten importieren“ den Mewgenics-Ordner wählen, beide Dateien per USB nach\n"
                + importDir(this).getAbsolutePath() + "\nkopieren, oder die Zusatzdateien in Plum Hub laden (landen in\n"
                + (obbDir(this) != null ? obbDir(this).getAbsolutePath() : "Android/obb/ch.plum.mewgenics")
                + ").\n\nSpielstände: " + userDir(this).getAbsolutePath());
        col.addView(help);

        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setBackgroundColor(Color.rgb(24, 18, 30));
        sv.addView(col);
        setContentView(sv);
        refresh();
    }

    @Override protected void onResume() {
        super.onResume();
        if (play != null && importer == null) refresh();   // e.g. after Plum Hub downloaded the data
    }

    private void refresh() {
        boolean ok = GameImporter.installed(gameDir(this));
        play.setEnabled(ok);
        if (ok) {
            File g = new File(gameDir(this), "resources.gpak");
            status.setText(String.format(Locale.ROOT, "Spieldaten gefunden (resources.gpak %.2f GB, %s).", g.length() / 1e9,
                    gameDir(this).equals(importDir(this)) ? "importiert" : "von Plum Hub"));
        } else {
            status.setText("Noch keine Spieldaten. Bitte importieren.");
        }
    }

    private void startGame() {
        startActivity(new Intent(this, MewActivity.class));
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, PICK_FOLDER);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != PICK_FOLDER || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        play.setEnabled(false);
        importBtn.setEnabled(false);
        bar.setVisibility(View.VISIBLE);
        importer = new GameImporter(getContentResolver(), importDir(this), (file, done, total, bps) -> ui.post(() -> {
            bar.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
            status.setText(String.format(Locale.ROOT, "Kopiere %s … %.2f / %.2f GB%s", file, done / 1e9, total / 1e9,
                    bps > 0 ? String.format(Locale.ROOT, " (%.0f MB/s)", bps / 1e6) : ""));
        }));
        new Thread(() -> {
            GameImporter.Result r = importer.importTree(tree);
            ui.post(() -> {
                importer = null;
                bar.setVisibility(View.GONE);
                importBtn.setEnabled(true);
                refresh();
                if (!r.ok || !GameImporter.installed(gameDir(this))) status.setText(r.message);
                else status.setText(r.message + " Bereit zum Spielen.");
            });
        }, "import").start();
    }
}
