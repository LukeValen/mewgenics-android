package ch.plum.mewgenics;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.StatFs;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Copies the player's own Mewgenics installation (Steam folder) into the app's storage.
 *
 * The APK carries no game data. The recompiled code was built from Mewgenics.exe of one
 * build (SHA-256 below); the exe image and resources.gpak (about 5.1 GB) come from the
 * player's copy, picked as a folder with the Storage Access Framework. Files already present
 * with the same size are kept, so an interrupted import continues where it stopped.
 */
final class GameImporter {
    static final String EXE_SHA256 = "4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea";

    interface Progress { void update(String file, long done, long total, long bytesPerSecond); }

    static final class Result {
        final boolean ok; final String message;
        Result(boolean ok, String message) { this.ok = ok; this.message = message; }
    }

    private final ContentResolver cr;
    private final File dest;
    private final Progress progress;
    private volatile boolean cancelled;
    private long done, total, t0, lastUi;
    private final byte[] buf = new byte[1 << 20];

    GameImporter(ContentResolver cr, File dest, Progress progress) { this.cr = cr; this.dest = dest; this.progress = progress; }
    void cancel() { cancelled = true; }

    static boolean wanted(String name) {
        String l = name.toLowerCase(Locale.ROOT);
        return l.equals("mewgenics.exe") || l.equals("resources.gpak");
    }

    static boolean installed(File dir) {
        return new File(dir, "Mewgenics.exe").isFile() && new File(dir, "resources.gpak").isFile();
    }

    static String sha256(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] b = new byte[1 << 16]; int n;
            while ((n = in.read(b)) > 0) md.update(b, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte x : md.digest()) sb.append(String.format(Locale.ROOT, "%02x", x));
            return sb.toString();
        } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    static long freeBytes(File dir) {
        dir.mkdirs();
        return new StatFs(dir.getAbsolutePath()).getAvailableBytes();
    }

    private static final class Doc { String id, name; long size; }

    /** Files directly in the picked folder, or in a sub folder named like the game. */
    private List<Doc> list(Uri tree, String docId, int depth, List<Doc> out) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        try (Cursor c = cr.query(children, new String[] {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE }, null, null, null)) {
            if (c == null) return out;
            while (c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (depth < 2) list(tree, id, depth + 1, out);
                } else if (wanted(name)) {
                    Doc d = new Doc(); d.id = id; d.name = name; d.size = c.isNull(3) ? 0 : c.getLong(3);
                    out.add(d);
                }
            }
        }
        return out;
    }

    Result importTree(Uri tree) {
        try {
            List<Doc> files = list(tree, DocumentsContract.getTreeDocumentId(tree), 0, new ArrayList<>());
            Doc exe = null, gpak = null;
            for (Doc d : files) {
                if (d.name.equalsIgnoreCase("Mewgenics.exe") && exe == null) exe = d;
                if (d.name.equalsIgnoreCase("resources.gpak") && gpak == null) gpak = d;
            }
            if (exe == null || gpak == null)
                return new Result(false, "In diesem Ordner fehlen Mewgenics.exe und/oder resources.gpak. Wähle den Mewgenics-Ordner deiner Steam-Installation.");
            total = exe.size + gpak.size;
            long need = 0;
            for (Doc d : new Doc[] { exe, gpak }) {
                File f = new File(dest, canonical(d.name));
                if (!(f.isFile() && f.length() == d.size)) need += d.size;
            }
            if (need > freeBytes(dest) - (300L << 20))
                return new Result(false, String.format(Locale.ROOT, "Zu wenig freier Speicher: %.1f GB nötig, %.1f GB frei.", need / 1e9, freeBytes(dest) / 1e9));
            t0 = System.nanoTime(); done = 0;
            for (Doc d : new Doc[] { exe, gpak }) {          // exe first: a wrong build stops early
                if (cancelled) return new Result(false, "Import abgebrochen.");
                File out = new File(dest, canonical(d.name));
                if (out.isFile() && out.length() == d.size) { done += d.size; tick(d.name); }
                else try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, d.id))) {
                    copy(in, out, d.name);
                }
                if (d == exe) {
                    String h = sha256(out);
                    if (!h.equals(EXE_SHA256)) {
                        out.delete();
                        return new Result(false, "Diese Mewgenics.exe stammt aus einer anderen Spielversion (SHA-256 " + h.substring(0, 12)
                                + "…). Der Port wurde aus der Version " + EXE_SHA256.substring(0, 12) + "… übersetzt.");
                    }
                }
            }
            return new Result(true, String.format(Locale.ROOT, "Import fertig: %.2f GB.", total / 1e9));
        } catch (Exception e) {
            return new Result(false, "Import fehlgeschlagen: " + e.getMessage());
        }
    }

    private static String canonical(String name) {
        return name.equalsIgnoreCase("Mewgenics.exe") ? "Mewgenics.exe" : "resources.gpak";
    }

    private void copy(InputStream in, File out, String rel) throws IOException {
        out.getParentFile().mkdirs();
        File part = new File(out.getPath() + ".part");
        try (OutputStream o = new FileOutputStream(part)) {
            int k;
            while ((k = in.read(buf)) > 0) {
                if (cancelled) throw new IOException("abgebrochen");
                o.write(buf, 0, k); done += k; tick(rel);
            }
        }
        if (!part.renameTo(out)) { out.delete(); if (!part.renameTo(out)) throw new IOException("kann nicht schreiben: " + out); }
    }

    private void tick(String rel) {
        long now = System.nanoTime();
        if (now - lastUi < 200_000_000L && done < total) return;
        lastUi = now;
        double s = (now - t0) / 1e9;
        progress.update(rel, done, total, s > 0.5 ? (long) (done / s) : 0);
    }
}
