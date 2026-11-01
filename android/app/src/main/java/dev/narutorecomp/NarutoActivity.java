package dev.narutorecomp;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewGroup;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * Runs the recompiled game: SDL's activity, with the game's main() in libmain.so
 * and SDL itself linked into librexruntime.so.
 *
 * Everything lives in the app's external files folder,
 * /sdcard/Android/data/dev.narutorecomp/files:
 *   game/        the user's own game files (default.xex and the rest)
 *   user/        settings (naruto.toml), saves and the shader cache
 *   naruto.log   the runtime log
 */
public class NarutoActivity extends SDLActivity {
    private static final String TAG = "NarutoRecomp";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // SDL's layout is only built when the libraries loaded.
        if (mLayout != null) {
            mLayout.addView(new TouchControlsView(this), new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    // SDL follows the phone's rotation for a resizable window; the game is
    // landscape only, either way up.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    @Override
    protected String[] getArguments() {
        File root = getExternalFilesDir(null);
        File game = new File(root, "game");
        File user = new File(root, "user");
        game.mkdirs();
        user.mkdirs();
        seedConfig(new File(user, "naruto.toml"));
        return new String[] {
            "--game_data_root=" + game.getAbsolutePath(),
            "--user_data_root=" + user.getAbsolutePath(),
            "--log_file=" + new File(root, "naruto.log").getAbsolutePath(),
            "--gpu_plugin=xenos",
            "--fullscreen=true",
        };
    }

    // The desktop build seeds naruto.toml from config/naruto-performance.toml;
    // do the same once, without overwriting settings saved later.
    private void seedConfig(File config) {
        if (config.exists()) {
            return;
        }
        try (InputStream in = getAssets().open("naruto.toml");
             OutputStream out = new FileOutputStream(config)) {
            byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
        } catch (IOException e) {
            Log.w(TAG, "Could not write default settings to " + config, e);
        }
    }
}
