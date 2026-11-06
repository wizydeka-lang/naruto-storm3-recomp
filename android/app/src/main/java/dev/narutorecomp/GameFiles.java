package dev.narutorecomp;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.File;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/**
 * Finds the user's copy of the game: a folder holding default.xex beside the
 * data/ folder, wherever it is on the phone's storage or an SD card. The game
 * opens its files by path, so the app needs access to all files.
 */
final class GameFiles {
    /** How many folders deep a search looks below each place it starts. */
    private static final int SEARCH_DEPTH = 5;

    private GameFiles() {}

    static boolean hasStorageAccess(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return context.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
    }

    /** default.xex and data/ in this folder (names in any case). */
    static boolean isGameFolder(File dir) {
        if (dir == null) {
            return false;
        }
        return child(dir, "default.xex", false) != null && child(dir, "data", true) != null;
    }

    /** The game's folder at or below dir, or null. */
    static File findIn(File dir, int depth) {
        ArrayDeque<File> queue = new ArrayDeque<>();
        ArrayDeque<Integer> depths = new ArrayDeque<>();
        queue.add(dir);
        depths.add(0);
        while (!queue.isEmpty()) {
            File current = queue.poll();
            int level = depths.poll();
            if (isGameFolder(current)) {
                return current;
            }
            if (level >= depth) {
                continue;
            }
            File[] children = current.listFiles();
            if (children == null) {
                continue;
            }
            for (File child : children) {
                String name = child.getName();
                // Other apps' private folders and hidden ones are never it.
                if (child.isDirectory() && !name.startsWith(".")
                        && !(level == 0 && name.equals("Android"))) {
                    queue.add(child);
                    depths.add(level + 1);
                }
            }
        }
        return null;
    }

    /** Searches the phone's storage and any SD card; null if not found. */
    static File search(Context context) {
        for (File root : storageRoots(context)) {
            File found = findIn(root, SEARCH_DEPTH);
            if (found != null) {
                return found;
            }
        }
        return null;
    }

    /** The top of each storage volume: the phone's, then SD cards. */
    static List<File> storageRoots(Context context) {
        List<File> roots = new ArrayList<>();
        roots.add(Environment.getExternalStorageDirectory());
        for (File dir : context.getExternalFilesDirs(null)) {
            if (dir == null) {
                continue;
            }
            // .../Android/data/<package>/files: the volume is above Android/.
            String path = dir.getAbsolutePath();
            int android = path.indexOf("/Android/data/");
            if (android > 0) {
                File root = new File(path.substring(0, android));
                if (!roots.contains(root)) {
                    roots.add(root);
                }
            }
        }
        return roots;
    }

    /**
     * The file system path of a folder picked with the system's folder
     * picker: "primary:Games/Naruto" is on the phone's storage, and
     * "1234-ABCD:Naruto" on the SD card of that name. Null if it's not one
     * of those (a cloud folder, say).
     */
    static File fromTreeUri(Uri uri) {
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(uri);
        } catch (IllegalArgumentException e) {
            return null;
        }
        int colon = id.indexOf(':');
        if (colon < 0) {
            return null;
        }
        String volume = id.substring(0, colon);
        String relative = id.substring(colon + 1);
        File root = volume.equalsIgnoreCase("primary")
                ? Environment.getExternalStorageDirectory()
                : new File("/storage/" + volume);
        return relative.isEmpty() ? root : new File(root, relative);
    }

    private static File child(File dir, String name, boolean directory) {
        File exact = new File(dir, name);
        if (directory ? exact.isDirectory() : exact.isFile()) {
            return exact;
        }
        String[] names = dir.list();
        if (names == null) {
            return null;
        }
        for (String n : names) {
            if (n.equalsIgnoreCase(name)) {
                File file = new File(dir, n);
                if (directory ? file.isDirectory() : file.isFile()) {
                    return file;
                }
            }
        }
        return null;
    }
}
