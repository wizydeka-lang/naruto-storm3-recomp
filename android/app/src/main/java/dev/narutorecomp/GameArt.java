package dev.narutorecomp;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * The game's dashboard artwork, read from its nxeart package: the 1920x1080
 * key art, the box art and the title icon. The package stores them as plain
 * PNG and JPEG files, so they are found by their signatures. The art comes
 * from the user's own copy of the game; none ships with the app.
 */
final class GameArt {
    /** The key art, or null. */
    Bitmap background;
    Bitmap boxArt;
    Bitmap icon;

    private static final byte[] PNG_START = {(byte) 0x89, 'P', 'N', 'G'};
    private static final byte[] PNG_END = {'I', 'E', 'N', 'D'};
    private static final byte[] JPEG_START = {(byte) 0xff, (byte) 0xd8, (byte) 0xff};
    private static final byte[] JPEG_END = {(byte) 0xff, (byte) 0xd9};

    /** The art in gameDir/nxeart; fields stay null for what's missing. */
    static GameArt load(File gameDir) {
        GameArt art = new GameArt();
        if (gameDir == null) {
            return art;
        }
        File file = new File(gameDir, "nxeart");
        if (!file.isFile() || file.length() > (16 << 20)) {
            return art;
        }
        byte[] data = new byte[(int) file.length()];
        try (InputStream in = new FileInputStream(file)) {
            int read = 0;
            while (read < data.length) {
                int n = in.read(data, read, data.length - read);
                if (n < 0) {
                    break;
                }
                read += n;
            }
        } catch (IOException e) {
            return art;
        }
        art.carve(data, PNG_START, PNG_END, 8);
        art.carve(data, JPEG_START, JPEG_END, 2);
        return art;
    }

    private void carve(byte[] data, byte[] startMarker, byte[] endMarker, int tail) {
        int start = indexOf(data, startMarker, 0);
        while (start >= 0) {
            int end = indexOf(data, endMarker, start);
            if (end < 0) {
                return;
            }
            int length = Math.min(end + tail, data.length) - start;
            Bitmap image = BitmapFactory.decodeByteArray(data, start, length);
            if (image != null) {
                keep(image);
            }
            start = indexOf(data, startMarker, end);
        }
    }

    // The first of each size class, as the desktop launcher picks them.
    private void keep(Bitmap image) {
        int width = image.getWidth();
        if (width >= 1280) {
            if (background == null) background = image;
        } else if (width >= 256) {
            if (boxArt == null) boxArt = image;
        } else if (icon == null) {
            icon = image;
        }
    }

    private static int indexOf(byte[] data, byte[] pattern, int from) {
        outer:
        for (int i = Math.max(from, 0); i <= data.length - pattern.length; i++) {
            for (int j = 0; j < pattern.length; j++) {
                if (data[i + j] != pattern[j]) {
                    continue outer;
                }
            }
            return i;
        }
        return -1;
    }
}
