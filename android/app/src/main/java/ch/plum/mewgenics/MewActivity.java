package ch.plum.mewgenics;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;

/** Runs the recompiled game: SDL3 hosts the window, input and audio; libmain.so is the game. */
public class MewActivity extends SDLActivity {
    /** Longest side of the render surface unless the player asked for full resolution. */
    static final int RENDER_MAX = 1920;

    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override protected String[] getArguments() {
        return new String[] { Launcher.gameDir(this).getAbsolutePath(), Launcher.userDir(this).getAbsolutePath() };
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (mSurface == null) return;
        // The game draws every frame at the surface size and some screens are fill-rate bound
        // (2960x1848 on a Tab S8 Ultra: 22 fps in the storage screen, 66 fps at 1920x1200).
        // A fixed, smaller buffer is scaled up by the compositor at no cost; SDLSurface maps
        // touch coordinates from the view to the buffer.
        SharedPreferences prefs = getSharedPreferences(Launcher.PREFS, MODE_PRIVATE);
        if (prefs.getBoolean(Launcher.PREF_FULL_RES, false)) return;
        DisplayMetrics dm = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(dm);
        int w = Math.max(dm.widthPixels, dm.heightPixels), h = Math.min(dm.widthPixels, dm.heightPixels);
        if (w <= RENDER_MAX) return;
        int sw = RENDER_MAX, sh = Math.round((float) h * RENDER_MAX / w);
        Log.i("Mewgenics", "render surface " + sw + "x" + sh + " (display " + w + "x" + h + ")");
        // SurfaceView measures itself at the fixed size under SDL's WRAP_CONTENT layout params;
        // MATCH_PARENT keeps the view full screen so the compositor scales the buffer up
        mSurface.setLayoutParams(new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        mSurface.getHolder().setFixedSize(sw, sh);
        nativeSetenv("W64_RENDER_SIZE", sw + "x" + sh);
    }
}
