package dev.narutorecomp;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * The launcher's settings, which the game's activity reads when it starts (in
 * its own process, so it sees what the launcher last saved).
 */
final class Settings {
    private static final String PREFS = "settings";

    /** The folder with default.xex and data/, as a file system path. */
    static final String GAME_DIR = "game_dir";
    /** "720p" or "1080p": the size the game renders at. */
    static final String RESOLUTION = "resolution";
    static final String FXAA = "fxaa";
    /** The on-screen Xbox 360 controller. */
    static final String TOUCH = "touch";
    /** Its size and opacity, 0.6-1.4 and 0.2-1. */
    static final String TOUCH_SIZE = "touch_size";
    static final String TOUCH_OPACITY = "touch_opacity";
    /** Every character playable, without unlocking them in Ultimate Adventure. */
    static final String UNLOCK_ALL = "unlock_all";

    static final String RES_720P = "720p";
    static final String RES_1080P = "1080p";

    private Settings() {}

    static SharedPreferences get(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    static String resolution(SharedPreferences prefs) {
        return RES_1080P.equals(prefs.getString(RESOLUTION, RES_720P)) ? RES_1080P : RES_720P;
    }

    /** The game renders at 1280x720 times this. */
    static float renderScale(SharedPreferences prefs) {
        return RES_1080P.equals(resolution(prefs)) ? 1.5f : 1.0f;
    }

    static boolean fxaa(SharedPreferences prefs) {
        return prefs.getBoolean(FXAA, false);
    }

    static boolean unlockAll(SharedPreferences prefs) {
        return prefs.getBoolean(UNLOCK_ALL, false);
    }

    static boolean touch(SharedPreferences prefs) {
        return prefs.getBoolean(TOUCH, true);
    }

    static float touchSize(SharedPreferences prefs) {
        return Math.max(0.6f, Math.min(1.4f, prefs.getFloat(TOUCH_SIZE, 1.0f)));
    }

    static float touchOpacity(SharedPreferences prefs) {
        return Math.max(0.2f, Math.min(1.0f, prefs.getFloat(TOUCH_OPACITY, 0.7f)));
    }
}
