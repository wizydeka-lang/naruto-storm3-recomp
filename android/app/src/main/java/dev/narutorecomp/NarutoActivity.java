package dev.narutorecomp;

import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.ViewGroup;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/**
 * Runs the recompiled game: SDL's activity, with the game's main() in libmain.so
 * and SDL itself linked into librexruntime.so. LauncherActivity starts it, in
 * a process of its own (the game's native state can't be started twice in
 * one process), with the settings it saved.
 *
 * The game's files are wherever the launcher found them. The app's external
 * files folder, /sdcard/Android/data/dev.narutorecomp/files, holds:
 *   user/        settings (naruto.toml), saves
 *   out/         the native renderer's shader and pipeline caches
 *   naruto.log   the runtime log
 *
 * The game draws through the native renderer only (native_view.h): it
 * renders at 720p or 1080p and the screen's compositor stretches that over
 * the whole screen, which costs the GPU nothing.
 */
public class NarutoActivity extends SDLActivity {
    private static final String TAG = "NarutoRecomp";

    private SharedPreferences prefs;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        prefs = Settings.get(this);
        // Before SDL loads the game: the renderer reads these at startup.
        configureRenderer();
        super.onCreate(savedInstanceState);
        configureWindow();
        // SDL's layout is only built when the libraries loaded.
        if (mLayout != null && Settings.touch(prefs)) {
            TouchControlsView controls = new TouchControlsView(this);
            controls.setLook(Settings.touchSize(prefs), Settings.touchOpacity(prefs));
            mLayout.addView(controls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // SDL can't start the game again in this process; the launcher starts
        // a new one next time.
        if (isFinishing()) {
            android.os.Process.killProcess(android.os.Process.myPid());
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
        File user = new File(root, "user");
        user.mkdirs();
        seedConfig(new File(user, "naruto.toml"));
        String game = prefs.getString(Settings.GAME_DIR, new File(root, "game").getAbsolutePath());
        return new String[] {
            "--game_data_root=" + game,
            "--user_data_root=" + user.getAbsolutePath(),
            "--log_file=" + new File(root, "naruto.log").getAbsolutePath(),
            "--gpu_plugin=xenos",
            "--fullscreen=true",
            // Xenia only runs the command stream; the native renderer draws.
            "--vulkan_skip_rendering=true",
            "--async_shader_compilation=true",
            "--vulkan_validation_enabled=false",
            "--gpu_debug_markers=false",
        };
    }

    private void configureRenderer() {
        File out = new File(getExternalFilesDir(null), "out");
        File shaders = new File(out, "shaders");
        shaders.mkdirs();
        File cache = new File(shaders, "naruto_shaders.bin");
        updateShaderCache(cache);
        setenv("NARUTO_NATIVE_VIEW", "1");
        setenv("NARUTO_NATIVE_VIEW_ONLY", "1");
        setenv("NARUTO_NATIVE_VIEW_SCALE", Settings.renderScale(prefs) == 1.5f ? "1.5" : "1");
        setenv("NARUTO_NATIVE_VIEW_FXAA", Settings.fxaa(prefs) ? "1" : "0");
        // Phones: the picture always fills the screen.
        setenv("NARUTO_NATIVE_VIEW_ASPECT", "fill");
        setenv("NARUTO_NATIVE_VIEW_UPSCALER", "linear");
        setenv("NARUTO_OUT_DIR", out.getAbsolutePath());
        setenv("NARUTO_SHADER_CACHE", cache.getAbsolutePath());
        // Unlocks are saved, so turning this off later keeps those characters.
        setenv("NARUTO_UNLOCK_ALL", Settings.unlockAll(prefs) ? "1" : "0");
        // For debugging: NAME=VALUE lines in env.txt beside naruto.log set
        // more variables (or override the ones above).
        File extra = new File(getExternalFilesDir(null), "env.txt");
        if (extra.isFile()) {
            try {
                for (String line : java.nio.file.Files.readAllLines(extra.toPath())) {
                    int equals = line.indexOf('=');
                    if (equals > 0 && !line.startsWith("#")) {
                        setenv(line.substring(0, equals).trim(), line.substring(equals + 1).trim());
                    }
                }
            } catch (java.io.IOException e) {
                Log.w(TAG, "Could not read " + extra, e);
            }
        }
    }

    // The power settings that keep a steady 30 frames a second for the least
    // battery: a refresh rate that 30 divides, the screen kept on, the
    // phone's sustained performance mode (steady clocks it can hold without
    // heating up), and the picture under any camera cutout.
    private void configureWindow() {
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = null;
        for (Display.Mode mode : display.getSupportedModes()) {
            float rate = mode.getRefreshRate();
            boolean multipleOf30 = Math.abs(rate / 30 - Math.round(rate / 30)) < 0.05f;
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight() && multipleOf30
                    && rate >= 29 && (best == null || rate < best.getRefreshRate())) {
                best = mode;
            }
        }
        if (best != null) {
            attributes.preferredDisplayModeId = best.getModeId();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            attributes.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        getWindow().setAttributes(attributes);
        PowerManager power = getSystemService(PowerManager.class);
        if (power != null && power.isSustainedPerformanceModeSupported()) {
            getWindow().setSustainedPerformanceMode(true);
        }
    }

    private static void setenv(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException e) {
            Log.w(TAG, "Could not set " + name, e);
        }
    }

    // The shader cache built on the PC (tools/build-shaders.py) comes in the
    // APK when it was there at build time; copied out once per install.
    private void updateShaderCache(File cache) {
        long installed;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            installed = info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            return;
        }
        if (cache.exists() && prefs.getLong("shader_cache_install", 0) == installed) {
            return;
        }
        // The shaders captured from memory on the PC (2D sprites: text, icons,
        // logos). The cache's SPIR-V for them is matched through these files,
        // so without them those draws are skipped until the phone captures
        // them itself.
        File runtime = new File(cache.getParentFile(), "runtime");
        runtime.mkdirs();
        try {
            String[] captured = getAssets().list("shader_runtime");
            if (captured != null) {
                for (String name : captured) {
                    copyAsset("shader_runtime/" + name, new File(runtime, name));
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "Could not list captured shaders", e);
        }
        if (copyAsset("naruto_shaders.bin", cache)) {
            prefs.edit().putLong("shader_cache_install", installed).apply();
        }
    }

    // The desktop build seeds naruto.toml from config/naruto-performance.toml;
    // do the same once, without overwriting settings saved later.
    private void seedConfig(File config) {
        if (!config.exists()) {
            copyAsset("naruto.toml", config);
        }
    }

    private boolean copyAsset(String name, File to) {
        try (InputStream in = getAssets().open(name);
             OutputStream out = new FileOutputStream(to)) {
            byte[] buffer = new byte[65536];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
            return true;
        } catch (IOException e) {
            Log.w(TAG, "Could not write " + name + " to " + to, e);
            return false;
        }
    }
}
