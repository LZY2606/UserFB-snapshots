package task_tests;

import bdsup2sub.bitmap.Bitmap;
import bdsup2sub.bitmap.Palette;
import bdsup2sub.core.CoreException;
import bdsup2sub.supstream.SubPicture;
import bdsup2sub.supstream.bd.SupBD;
import bdsup2sub.supstream.bd.SupBDWriter;
import task_support.Checks;

import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * Regression tests for the dual object PGS implementation.
 * All input streams are constructed independently in this file; no demo
 * reader is involved.
 */
public final class PGSCompositionTest {

    private static final int PDS = 0x14;
    private static final int ODS = 0x15;
    private static final int PCS = 0x16;
    private static final int WDS = 0x17;
    private static final int END = 0x80;

    private static final int STATE_EPOCH_START = 0x80;
    private static final int STATE_NORMAL = 0x00;

    private PGSCompositionTest() {
    }

    /* ================================================================== */
    /* objects: fragment interleaving, cropping, clipping, completeness   */
    /* ================================================================== */

    public static void objects(Checks c) throws Exception {
        c.runCase("interleaved fragments compose both objects", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(5, 0, 1, 0, false, 0, 0, 0, 0, false),
                        ref(9, 1, 5, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4), window(1, 4, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                byte[] rle5 = encodeRLE(fill(new Bitmap(2, 2), (byte) 1));
                byte[] rle9 = encodeRLE(fill(new Bitmap(2, 2), (byte) 2));
                int mid5 = rle5.length / 2;
                int mid9 = rle9.length / 2;
                // fragments of the two objects are interleaved
                b.odsFirst(90000, 5, 0, 2, 2, rle5.length + 4, slice(rle5, 0, mid5));
                b.odsFirst(90000, 9, 0, 2, 2, rle9.length + 4, slice(rle9, 0, mid9));
                b.odsLast(90000, 5, 0, slice(rle5, mid5, rle5.length));
                b.odsLast(90000, 9, 0, slice(rle9, mid9, rle9.length));
                b.end(90000);
                b.zeroPcs(180000, 1);
                b.wds(180000, window(0, 0, 0, 4, 4));
                b.end(180000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 1, "one caption frame expected");
                c.check(sup.getStartTime(0) == 90000, "start PTS");
                c.check(sup.getEndTime(0) == 180000, "end PTS");
                SubPicture pic = sup.getSubPicture(0);
                c.check(pic.getXOffset() == 1, "bounding box x offset");
                c.check(pic.getYOffset() == 0, "bounding box y offset");
                c.check(pic.getImageWidth() == 6, "bounding box width");
                c.check(pic.getImageHeight() == 2, "bounding box height");

                sup.decode(0);
                Bitmap bm = sup.getBitmap();
                c.check(bm.getWidth() == 6 && bm.getHeight() == 2, "decoded bitmap size");
                byte[] px = bm.getInternalBuffer();
                c.check((px[0] & 0xff) == 1, "object 5 pixel");
                c.check((px[1] & 0xff) == 1, "object 5 pixel 2");
                c.check((px[2] & 0xff) == 0, "gap stays transparent");
                c.check((px[4] & 0xff) == 2, "object 9 pixel");
                c.check((px[5] & 0xff) == 2, "object 9 pixel 2");
                c.check(sup.getPalette().getAlpha(1) == 255, "opaque palette entry");
                sup.close();
            }
        });

        c.runCase("crop rectangle is clipped against window and canvas", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                Bitmap obj = fill(new Bitmap(4, 4), (byte) 1);
                obj.getInternalBuffer()[1 + 4] = 2; // distinctive pixel at (1,1)
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(7, 0, 2, 1, true, 1, 1, 2, 2, false));
                b.wds(90000, window(0, 0, 0, 3, 2));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 7, 0, 4, 4, encodeRLE(obj));
                b.end(90000);
                b.zeroPcs(180000, 1);
                b.wds(180000, window(0, 0, 0, 3, 2));
                b.end(180000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 1, "one frame");
                SubPicture pic = sup.getSubPicture(0);
                // cropped 2x2 rect at (2,1) intersects window (0,0,3,2) -> 1x1 at (2,1)
                c.check(pic.getXOffset() == 2 && pic.getYOffset() == 1, "clipped position");
                c.check(pic.getImageWidth() == 1 && pic.getImageHeight() == 1, "clipped size");
                sup.decode(0);
                c.check((sup.getBitmap().getInternalBuffer()[0] & 0xff) == 2, "cropped source pixel");
                sup.close();
            }
        });

        c.runCase("references without geometric intersection add no frame", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(3, 0, 5, 5, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 1, 1));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 3, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.end(90000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 0, "no frame without intersection");
                sup.close();
            }
        });

        c.runCase("crop rectangle extending past the object is clamped to its bounds", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                Bitmap obj = fill(new Bitmap(4, 2), (byte) 1);
                obj.getInternalBuffer()[3 + 4] = 2; // distinctive pixel at (3,1)
                // crop origin (3,1) with an oversized 3x3 crop inside a 4x2 object
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(7, 0, 0, 0, true, 3, 1, 3, 3, false));
                b.wds(90000, window(0, 0, 0, 8, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 7, 0, 4, 2, encodeRLE(obj));
                b.end(90000);
                b.zeroPcs(180000, 1);
                b.wds(180000, window(0, 0, 0, 8, 4));
                b.end(180000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 1, "one frame");
                SubPicture pic = sup.getSubPicture(0);
                // clamped to the 1x1 region (3,1)..(4,2) of the object
                c.check(pic.getXOffset() == 0 && pic.getYOffset() == 0, "clamped position");
                c.check(pic.getImageWidth() == 1 && pic.getImageHeight() == 1, "clamped size");
                sup.decode(0);
                c.check((sup.getBitmap().getInternalBuffer()[0] & 0xff) == 2, "clamped source pixel");
                sup.close();
            }
        });

        c.runCase("reference to an object that was never defined throws", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(3, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.end(90000);
                boolean thrown = false;
                try {
                    read(b);
                } catch (CoreException ex) {
                    thrown = true;
                }
                c.check(thrown, "CoreException for incomplete object");
            }
        });

        c.runCase("forced flag is the OR of the visible objects", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(5, 0, 0, 0, false, 0, 0, 0, 0, false),
                        ref(9, 1, 4, 0, false, 0, 0, 0, 0, true));
                b.wds(90000, window(0, 0, 0, 4, 4), window(1, 4, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 5, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.odsSingle(90000, 9, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 2)));
                b.end(90000);
                b.zeroPcs(180000, 1);
                b.wds(180000, window(0, 0, 0, 4, 4));
                b.end(180000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 1, "one frame");
                c.check(sup.isForced(0), "forced OR of visible objects");
                sup.close();
            }
        });
    }

    /* ================================================================== */
    /* snapshots: object cache, interval boundaries, versions, epochs     */
    /* ================================================================== */

    public static void snapshots(Checks c) throws Exception {
        c.runCase("later PCS reuses a completed object without another ODS", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 1, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.end(90000);
                // same composition, no ODS/PDS: same interval
                b.pcs(120000, STATE_NORMAL, 1,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(120000, window(0, 0, 0, 4, 4));
                b.end(120000);
                // position changes without ODS: new interval
                b.pcs(150000, STATE_NORMAL, 2,
                        ref(1, 0, 3, 0, false, 0, 0, 0, 0, false));
                b.wds(150000, window(0, 0, 0, 4, 4));
                b.end(150000);
                b.zeroPcs(180000, 3);
                b.wds(180000, window(0, 0, 0, 4, 4));
                b.end(180000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 2, "two intervals");
                c.check(sup.getStartTime(0) == 90000 && sup.getEndTime(0) == 150000, "first interval timing");
                c.check(sup.getStartTime(1) == 150000 && sup.getEndTime(1) == 180000, "second interval timing");

                sup.decode(1);
                c.check(sup.getSubPicture(1).getXOffset() == 3, "reused object at new position");
                c.check((sup.getBitmap().getInternalBuffer()[0] & 0xff) == 1, "reused object pixels");

                sup.decode(0);
                c.check(sup.getSubPicture(0).getXOffset() == 0, "earlier frame snapshot unchanged");
                c.check((sup.getBitmap().getInternalBuffer()[0] & 0xff) == 1, "earlier frame pixels unchanged");
                sup.close();
            }
        });

        c.runCase("an incomplete new object version cannot borrow the old pixels", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 1, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.end(90000);
                // version 1 is started but never completed (no last fragment)
                b.pcs(120000, STATE_NORMAL, 1,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(120000, window(0, 0, 0, 4, 4));
                byte[] rle = encodeRLE(fill(new Bitmap(2, 1), (byte) 2));
                b.odsFirst(120000, 1, 1, 2, 1, rle.length + 4, rle);
                b.end(120000);

                boolean thrown = false;
                try {
                    read(b);
                } catch (CoreException ex) {
                    thrown = true;
                }
                c.check(thrown, "CoreException instead of reusing stale version");
            }
        });

        c.runCase("palette updates start a new interval and old frames keep their palette", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4));
                b.pds(90000, 0, 0, paletteWithLuma(235));
                b.odsSingle(90000, 1, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.end(90000);
                // identical composition, only a palette update -> new interval
                b.pcs(120000, STATE_NORMAL, 1,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(120000, window(0, 0, 0, 4, 4));
                b.pds(120000, 0, 1, paletteWithLuma(100));
                b.end(120000);
                b.zeroPcs(150000, 2);
                b.wds(150000, window(0, 0, 0, 4, 4));
                b.end(150000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 2, "palette change splits intervals");
                sup.decode(0);
                c.check((sup.getPalette().getY()[1] & 0xff) == 235, "first frame keeps old palette");
                sup.decode(1);
                c.check((sup.getPalette().getY()[1] & 0xff) == 100, "second frame uses new palette");
                sup.decode(0);
                c.check((sup.getPalette().getY()[1] & 0xff) == 235, "first frame palette snapshot stable");
                sup.close();
            }
        });

        c.runCase("a new epoch does not inherit the previous object cache", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 1, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.end(90000);
                b.zeroPcs(120000, 1);
                b.wds(120000, window(0, 0, 0, 4, 4));
                b.end(120000);
                // new epoch references object 1 again but carries no ODS
                b.pcs(150000, STATE_EPOCH_START, 2,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(150000, window(0, 0, 0, 4, 4));
                b.pds(150000, 0, 0, defaultPaletteEntries());
                b.end(150000);

                boolean thrown = false;
                try {
                    read(b);
                } catch (CoreException ex) {
                    thrown = true;
                }
                c.check(thrown, "CoreException: new epoch must not inherit objects");
            }
        });

        c.runCase("a zero object PCS ends the current caption", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(1, 0, 0, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 1, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.end(90000);
                b.zeroPcs(180000, 1);
                b.wds(180000, window(0, 0, 0, 4, 4));
                b.end(180000);

                SupBD sup = read(b);
                c.check(sup.getFrameCount() == 1, "single caption");
                c.check(sup.getStartTime(0) == 90000, "start");
                c.check(sup.getEndTime(0) == 180000, "end set by zero object PCS");
                sup.close();
            }
        });
    }

    /* ================================================================== */
    /* roundtrip: SupBDWriter output is read back identically             */
    /* ================================================================== */

    public static void roundtrip(Checks c) throws Exception {
        c.runCase("dual object composition survives write and re-read", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(5, 0, 1, 0, false, 0, 0, 0, 0, false),
                        ref(9, 1, 5, 0, false, 0, 0, 0, 0, false));
                b.wds(90000, window(0, 0, 0, 4, 4), window(1, 4, 0, 4, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 5, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 1)));
                b.odsSingle(90000, 9, 0, 2, 1, encodeRLE(fill(new Bitmap(2, 1), (byte) 2)));
                b.end(90000);
                b.zeroPcs(180000, 1);
                b.wds(180000, window(0, 0, 0, 4, 4));
                b.end(180000);

                SupBD source = read(b);
                c.check(source.getFrameCount() == 1, "source frame");
                source.decode(0);
                Bitmap sourceBitmap = source.getBitmap();
                Palette sourcePalette = source.getPalette();
                SubPicture sourcePic = source.getSubPicture(0);
                sourcePic.setCompositionNumber(0);

                byte[] written = SupBDWriter.createSupFrame(sourcePic, sourceBitmap, sourcePalette);
                Path out = tempFile("pgs-roundtrip-", ".sup");
                Files.write(out, written);
                SupBD reread = new SupBD(out.toString());

                c.check(reread.getFrameCount() == 1, "one frame after re-read");
                c.check(reread.getStartTime(0) == 90000, "90kHz start preserved");
                c.check(reread.getEndTime(0) == 180000, "90kHz end preserved");
                SubPicture rereadPic = reread.getSubPicture(0);
                c.check(rereadPic.getXOffset() == sourcePic.getXOffset(), "absolute x preserved");
                c.check(rereadPic.getYOffset() == sourcePic.getYOffset(), "absolute y preserved");
                c.check(rereadPic.getImageWidth() == sourcePic.getImageWidth(), "width preserved");
                c.check(rereadPic.getImageHeight() == sourcePic.getImageHeight(), "height preserved");

                reread.decode(0);
                Bitmap rereadBitmap = reread.getBitmap();
                c.check(rereadBitmap.getWidth() == sourceBitmap.getWidth(), "bitmap width");
                c.check(rereadBitmap.getHeight() == sourceBitmap.getHeight(), "bitmap height");
                byte[] a = sourceBitmap.getInternalBuffer();
                byte[] d = rereadBitmap.getInternalBuffer();
                boolean samePixels = a.length == d.length;
                for (int i = 0; samePixels && i < a.length; i++) {
                    samePixels = (a[i] & 0xff) == (d[i] & 0xff);
                }
                c.check(samePixels, "all palette indices identical");
                boolean sameColors = true;
                for (int i = 0; i < 3; i++) {
                    sameColors &= sourcePalette.getARGB(i) == reread.getPalette().getARGB(i);
                }
                c.check(sameColors, "palette colors identical");
                source.close();
                reread.close();
            }
        });

        c.runCase("cropped single object roundtrip keeps position and timing", new Checks.CaseBody() {
            public void run() throws Exception {
                SupBuilder b = new SupBuilder(8, 4);
                Bitmap obj = fill(new Bitmap(4, 4), (byte) 1);
                b.pcs(90000, STATE_EPOCH_START, 0,
                        ref(7, 0, 2, 1, true, 1, 1, 2, 2, false));
                b.wds(90000, window(0, 0, 0, 8, 4));
                b.pds(90000, 0, 0, defaultPaletteEntries());
                b.odsSingle(90000, 7, 0, 4, 4, encodeRLE(obj));
                b.end(90000);
                b.zeroPcs(270000, 1);
                b.wds(270000, window(0, 0, 0, 8, 4));
                b.end(270000);

                SupBD source = read(b);
                source.decode(0);
                SubPicture pic = source.getSubPicture(0);
                pic.setCompositionNumber(0);
                byte[] written = SupBDWriter.createSupFrame(pic, source.getBitmap(), source.getPalette());
                Path out = tempFile("pgs-crop-roundtrip-", ".sup");
                Files.write(out, written);
                SupBD reread = new SupBD(out.toString());

                c.check(reread.getStartTime(0) == 90000 && reread.getEndTime(0) == 270000, "timing preserved");
                c.check(reread.getSubPicture(0).getXOffset() == 2, "cropped x preserved");
                c.check(reread.getSubPicture(0).getYOffset() == 1, "cropped y preserved");
                c.check(reread.getSubPicture(0).getImageWidth() == 2, "cropped width preserved");
                c.check(reread.getSubPicture(0).getImageHeight() == 2, "cropped height preserved");
                reread.decode(0);
                c.check((reread.getBitmap().getInternalBuffer()[0] & 0xff) == 1, "pixel preserved");
                source.close();
                reread.close();
            }
        });
    }

    /* ================================================================== */
    /* helpers                                                            */
    /* ================================================================== */

    private static SupBD read(SupBuilder b) throws Exception {
        Path file = tempFile("pgs-test-", ".sup");
        Files.write(file, b.toBytes());
        return new SupBD(file.toString());
    }

    /**
     * Create a temp file in $TMPDIR when set (sandboxed environments may only
     * permit that directory), otherwise in the JVM default temp directory.
     */
    private static Path tempFile(String prefix, String suffix) throws Exception {
        String dir = System.getenv("TMPDIR");
        if (dir != null && !dir.isEmpty()) {
            return Files.createTempFile(Path.of(dir), prefix, suffix);
        }
        return Files.createTempFile(prefix, suffix);
    }

    private static Bitmap fill(Bitmap bm, byte index) {
        byte[] buf = bm.getInternalBuffer();
        for (int i = 0; i < buf.length; i++) {
            buf[i] = index;
        }
        return bm;
    }

    /**
     * Minimal PGS RLE encoder: zero pixels as 00 01, other pixels as raw
     * bytes, each row terminated by 00 00. Sufficient for the small test
     * bitmaps and decodable by the production RLE decoder.
     */
    private static byte[] encodeRLE(Bitmap bm) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = bm.getInternalBuffer();
        int w = bm.getWidth();
        for (int y = 0; y < bm.getHeight(); y++) {
            for (int x = 0; x < w; x++) {
                int v = buf[y * w + x] & 0xff;
                if (v == 0) {
                    out.write(0);
                    out.write(1);
                } else {
                    out.write(v);
                }
            }
            out.write(0);
            out.write(0);
        }
        return out.toByteArray();
    }

    private static byte[] slice(byte[] data, int from, int to) {
        byte[] r = new byte[to - from];
        System.arraycopy(data, from, r, 0, r.length);
        return r;
    }

    /** palette entries: index 0 transparent, 1..3 opaque with distinct luma */
    private static int[][] defaultPaletteEntries() {
        return new int[][]{
                {0, 16, 128, 128, 0},
                {1, 235, 128, 128, 255},
                {2, 160, 128, 128, 255},
                {3, 90, 128, 128, 255}
        };
    }

    private static int[][] paletteWithLuma(int luma) {
        return new int[][]{
                {0, 16, 128, 128, 0},
                {1, luma, 128, 128, 255}
        };
    }

    private static int[] ref(int objectId, int windowId, int x, int y, boolean cropped,
                             int cropX, int cropY, int cropW, int cropH, boolean forced) {
        return new int[]{objectId, windowId, x, y, cropped ? 1 : 0, cropX, cropY, cropW, cropH, forced ? 1 : 0};
    }

    private static int[] window(int id, int x, int y, int w, int h) {
        return new int[]{id, x, y, w, h};
    }

    /** Builds a PGS byte stream from segments. */
    private static final class SupBuilder {
        private final int videoWidth;
        private final int videoHeight;
        private final List<byte[]> segments = new ArrayList<byte[]>();

        SupBuilder(int videoWidth, int videoHeight) {
            this.videoWidth = videoWidth;
            this.videoHeight = videoHeight;
        }

        byte[] toBytes() {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            for (byte[] s : segments) {
                out.write(s, 0, s.length);
            }
            return out.toByteArray();
        }

        private void addSegment(int type, long pts, byte[] body) {
            byte[] s = new byte[13 + body.length];
            s[0] = 0x50;
            s[1] = 0x47;
            writeDWord(s, 2, (int) pts);
            writeDWord(s, 6, 0);
            s[10] = (byte) type;
            writeWord(s, 11, body.length);
            System.arraycopy(body, 0, s, 13, body.length);
            segments.add(s);
        }

        void pcs(long pts, int state, int compositionNumber, int[]... refs) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            writeWord(out, videoWidth);
            writeWord(out, videoHeight);
            out.write(0x10); // frame rate
            writeWord(out, compositionNumber);
            out.write(state);
            out.write(0); // palette update flag
            out.write(0); // palette id
            out.write(refs.length);
            for (int[] r : refs) {
                writeWord(out, r[0]); // object id
                out.write(r[1]);      // window id
                int flags = (r[4] != 0 ? 0x80 : 0) | (r[9] != 0 ? 0x40 : 0);
                out.write(flags);
                writeWord(out, r[2]); // x
                writeWord(out, r[3]); // y
                if (r[4] != 0) {
                    writeWord(out, r[5]);
                    writeWord(out, r[6]);
                    writeWord(out, r[7]);
                    writeWord(out, r[8]);
                }
            }
            addSegment(PCS, pts, out.toByteArray());
        }

        void zeroPcs(long pts, int compositionNumber) {
            pcs(pts, STATE_NORMAL, compositionNumber);
        }

        void wds(long pts, int[]... windows) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(windows.length);
            for (int[] w : windows) {
                out.write(w[0]);
                writeWord(out, w[1]);
                writeWord(out, w[2]);
                writeWord(out, w[3]);
                writeWord(out, w[4]);
            }
            addSegment(WDS, pts, out.toByteArray());
        }

        void pds(long pts, int paletteId, int version, int[][] entries) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(paletteId);
            out.write(version);
            for (int[] e : entries) {
                for (int v : e) {
                    out.write(v & 0xff);
                }
            }
            addSegment(PDS, pts, out.toByteArray());
        }

        void odsSingle(long pts, int objectId, int version, int width, int height, byte[] rle) {
            addSegment(ODS, pts, odsBody(objectId, version, 0xC0, width, height, rle.length + 4, rle));
        }

        void odsFirst(long pts, int objectId, int version, int width, int height,
                      int declaredLength, byte[] fragment) {
            addSegment(ODS, pts, odsBody(objectId, version, 0x80, width, height, declaredLength, fragment));
        }

        void odsLast(long pts, int objectId, int version, byte[] fragment) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            writeWord(out, objectId);
            out.write(version);
            out.write(0x40);
            out.write(fragment, 0, fragment.length);
            addSegment(ODS, pts, out.toByteArray());
        }

        private byte[] odsBody(int objectId, int version, int sequence, int width, int height,
                               int declaredLength, byte[] payload) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            writeWord(out, objectId);
            out.write(version);
            out.write(sequence);
            out.write((declaredLength >> 16) & 0xff);
            out.write((declaredLength >> 8) & 0xff);
            out.write(declaredLength & 0xff);
            writeWord(out, width);
            writeWord(out, height);
            out.write(payload, 0, payload.length);
            return out.toByteArray();
        }

        void end(long pts) {
            addSegment(END, pts, new byte[0]);
        }
    }

    private static void writeWord(byte[] b, int ofs, int v) {
        b[ofs] = (byte) ((v >> 8) & 0xff);
        b[ofs + 1] = (byte) (v & 0xff);
    }

    private static void writeDWord(byte[] b, int ofs, int v) {
        b[ofs] = (byte) ((v >> 24) & 0xff);
        b[ofs + 1] = (byte) ((v >> 16) & 0xff);
        b[ofs + 2] = (byte) ((v >> 8) & 0xff);
        b[ofs + 3] = (byte) (v & 0xff);
    }

    private static void writeWord(ByteArrayOutputStream out, int v) {
        out.write((v >> 8) & 0xff);
        out.write(v & 0xff);
    }
}
