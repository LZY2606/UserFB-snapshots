package bdsup2sub.supstream.bd;

import bdsup2sub.core.Core;
import bdsup2sub.core.CoreException;
import bdsup2sub.core.Logger;
import bdsup2sub.supstream.ImageObjectFragment;
import bdsup2sub.supstream.PaletteInfo;
import bdsup2sub.tools.FileBuffer;
import bdsup2sub.tools.FileBufferException;
import bdsup2sub.utils.ToolBox;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import static bdsup2sub.utils.TimeUtils.ptsToTimeStr;

/**
 * Parser for Blu-Ray PGS (BD-SUP) streams supporting display sets with up to
 * two composition objects and two windows.
 *
 * Object definitions (ODS) and the display references (PCS) are managed
 * independently. Completed objects are cached for the duration of an epoch and
 * later PCS segments may reference them without another ODS; fragments of
 * different objects may be interleaved. A new object version replaces the
 * cached definition, while frames keep a reference to the exact version they
 * display. A new epoch starts with an empty object and palette cache.
 *
 * Each END segment commits one display set. A new caption interval is started
 * whenever the references, their position, the windows or the palette change,
 * even if the display set carries no new ODS. A PCS without composition
 * objects ends the current caption. Already committed frames are snapshots
 * and are never modified by later object or palette updates.
 */
public class SupBDParser {

    private static final Logger logger = Logger.getInstance();

    private static final int PGSSUP_FILE_MAGIC = 0x5047;
    private static final int PGSSUP_PALETTE_SEGMENT = 0x14;
    private static final int PGSSUP_PICTURE_SEGMENT = 0x15;
    private static final int PGSSUP_PRESENTATION_SEGMENT = 0x16;
    private static final int PGSSUP_WINDOW_SEGMENT = 0x17;
    private static final int PGSSUP_DISPLAY_SEGMENT = 0x80;

    private static class Segment {
        int type;
        int size;
        long pts;
        int offset; // file offset of segment body
    }

    /** Integer rectangle on the video canvas. */
    private static class Rect {
        int x;
        int y;
        int width;
        int height;

        Rect(int x, int y, int width, int height) {
            this.x = x;
            this.y = y;
            this.width = width;
            this.height = height;
        }

        boolean isEmpty() {
            return width <= 0 || height <= 0;
        }
    }

    private FileBuffer buffer;
    private final List<SubPictureBD> subPictures = new ArrayList<SubPictureBD>();
    private int forcedFrameCount;

    /** object cache of the current epoch, keyed by the 16 bit object id */
    private Map<Integer, PGSObject> objectDefinitions = new LinkedHashMap<Integer, PGSObject>();
    /** cumulative palette updates of the current epoch, one list per palette id */
    private List<List<PaletteInfo>> epochPalettes = newPaletteLists();

    /** frame described by the PCS of the display set which has not been committed yet */
    private SubPictureBD pendingFrame;
    /** last committed frame, used for interval detection and end time stamps */
    private SubPictureBD lastFrame;
    /** an ODS was seen in the current display set */
    private boolean odsInDisplaySet;

    public SupBDParser(String filename) throws CoreException {
        try {
            buffer = new FileBuffer(filename);
        } catch (FileBufferException ex) {
            throw new CoreException(ex.getMessage());
        }
        parse();
    }

    private static List<List<PaletteInfo>> newPaletteLists() {
        List<List<PaletteInfo>> lists = new ArrayList<List<PaletteInfo>>(8);
        for (int i = 0; i < 8; i++) {
            lists.add(new ArrayList<PaletteInfo>());
        }
        return lists;
    }

    private void parse() throws CoreException {
        int index = 0;
        long bufferSize = buffer.getSize();

        try {
            while (index < bufferSize) {
                if (Core.isCanceled()) {
                    throw new CoreException("Canceled by user!");
                }
                Core.setProgress(index);
                Segment segment = readSegment(index);
                switch (segment.type) {
                    case PGSSUP_PALETTE_SEGMENT:
                        parsePDS(segment);
                        break;
                    case PGSSUP_PICTURE_SEGMENT:
                        parseODS(segment);
                        break;
                    case PGSSUP_PRESENTATION_SEGMENT:
                        parsePCS(segment);
                        break;
                    case PGSSUP_WINDOW_SEGMENT:
                        parseWDS(segment);
                        break;
                    case PGSSUP_DISPLAY_SEGMENT:
                        commitDisplaySet(segment);
                        break;
                    default:
                        logger.warn("<unknown> " + ToolBox.toHexLeftZeroPadded(segment.type, 2)
                                + " ofs:" + ToolBox.toHexLeftZeroPadded(index, 8) + "\n");
                        break;
                }
                index += 13; // header size
                index += segment.size;
            }
        } catch (FileBufferException ex) {
            throw new CoreException(ex.getMessage());
        }
        Core.setProgress(bufferSize);
        countForcedFrames();
    }

    private Segment readSegment(int offset) throws FileBufferException, CoreException {
        Segment segment = new Segment();
        if (buffer.getWord(offset) != PGSSUP_FILE_MAGIC) {
            throw new CoreException("PG missing at index " + ToolBox.toHexLeftZeroPadded(offset, 8) + "\n");
        }
        segment.pts = buffer.getDWord(offset + 2);
        offset += 6; /* skip PTS and DTS */
        segment.type = buffer.getByte(offset += 4);
        segment.size = buffer.getWord(offset += 1);
        segment.offset = offset + 2;
        return segment;
    }

    private void parsePCS(Segment segment) throws FileBufferException, CoreException {
        int index = segment.offset;
        if (segment.size < 11) {
            throw new CoreException("Illegal PCS segment size " + segment.size
                    + " at offset " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
        }

        int videoWidth = buffer.getWord(index);
        int videoHeight = buffer.getWord(index + 2);
        int type = buffer.getByte(index + 4);
        int compositionNumber = buffer.getWord(index + 5);
        int stateValue = buffer.getByte(index + 7);
        PGSCompositionState state = PGSCompositionState.INVALID;
        for (PGSCompositionState s : PGSCompositionState.values()) {
            if (stateValue == s.getType()) {
                state = s;
            }
        }
        if (state == PGSCompositionState.INVALID) {
            throw new CoreException("Illegal composition state at offset "
                    + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
        }
        boolean paletteUpdate = (buffer.getByte(index + 8) & 0x80) == 0x80;
        int paletteId = buffer.getByte(index + 9);
        int objectCount = buffer.getByte(index + 10);
        if (objectCount > 2) {
            throw new CoreException("Unsupported number of composition objects: " + objectCount);
        }

        if (state == PGSCompositionState.EPOCH_START) {
            // a new epoch clears the screen and does not inherit object definitions or palettes
            if (lastFrame != null) {
                lastFrame.setEndTime(segment.pts);
            }
            objectDefinitions = new LinkedHashMap<Integer, PGSObject>();
            epochPalettes = newPaletteLists();
            lastFrame = null;
            logger.info("#> epoch start (" + ptsToTimeStr(segment.pts) + ")\n");
        }

        SubPictureBD frame = new SubPictureBD();
        frame.setWidth(videoWidth);
        frame.setHeight(videoHeight);
        frame.setType(type);
        frame.setPaletteId(paletteId);
        frame.setCompositionNumber(compositionNumber);
        frame.setStartTime(segment.pts);
        // the frame starts with a snapshot of all palette updates seen in this epoch
        for (int i = 0; i < epochPalettes.size(); i++) {
            frame.getPalettes().get(i).addAll(epochPalettes.get(i));
        }

        int pos = index + 11;
        for (int i = 0; i < objectCount; i++) {
            if (pos + 8 > segment.offset + segment.size) {
                throw new CoreException("Truncated composition object in PCS at offset "
                        + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
            }
            int objectId = buffer.getWord(pos);
            int windowId = buffer.getByte(pos + 2);
            int flags = buffer.getByte(pos + 3);
            boolean cropped = (flags & 0x80) == 0x80;
            boolean forced = (flags & 0x40) == 0x40;
            int x = buffer.getWord(pos + 4);
            int y = buffer.getWord(pos + 6);
            pos += 8;
            int cropX = 0, cropY = 0, cropWidth = 0, cropHeight = 0;
            if (cropped) {
                if (pos + 8 > segment.offset + segment.size) {
                    throw new CoreException("Truncated cropping rectangle in PCS at offset "
                            + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
                }
                cropX = buffer.getWord(pos);
                cropY = buffer.getWord(pos + 2);
                cropWidth = buffer.getWord(pos + 4);
                cropHeight = buffer.getWord(pos + 6);
                pos += 8;
            }
            frame.getCompositionRefs().add(new CompositionObjectRef(objectId, windowId, x, y,
                    cropped, cropX, cropY, cropWidth, cropHeight, forced));
        }

        pendingFrame = frame;
        odsInDisplaySet = false;

        logger.trace("PCS offset: " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8)
                + ", state: " + state + ", comp: " + compositionNumber
                + ", objects: " + objectCount + ", pal update: " + paletteUpdate
                + ", pts: " + ptsToTimeStr(segment.pts) + "\n");
    }

    private void parseWDS(Segment segment) throws FileBufferException, CoreException {
        int index = segment.offset;
        if (pendingFrame == null) {
            logger.warn("WDS without PCS at offset "
                    + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8) + " -> ignored\n");
            return;
        }
        if (segment.size < 1) {
            throw new CoreException("Illegal WDS segment size " + segment.size);
        }
        int windowCount = buffer.getByte(index);
        if (windowCount > 2) {
            throw new CoreException("Unsupported number of windows: " + windowCount);
        }
        int pos = index + 1;
        for (int i = 0; i < windowCount; i++) {
            if (pos + 9 > segment.offset + segment.size) {
                throw new CoreException("Truncated window definition in WDS at offset "
                        + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
            }
            int windowId = buffer.getByte(pos);
            int x = buffer.getWord(pos + 1);
            int y = buffer.getWord(pos + 3);
            int width = buffer.getWord(pos + 5);
            int height = buffer.getWord(pos + 7);
            pos += 9;
            pendingFrame.getWindows().put(windowId, new WindowDefinition(windowId, x, y, width, height));
        }
    }

    private void parseODS(Segment segment) throws FileBufferException, CoreException {
        int index = segment.offset;
        if (segment.size < 4) {
            throw new CoreException("Illegal ODS segment size " + segment.size);
        }
        int objectId = buffer.getWord(index);
        int objectVersion = buffer.getByte(index + 2);
        int sequenceFlags = buffer.getByte(index + 3);
        boolean first = (sequenceFlags & 0x80) == 0x80;
        boolean last = (sequenceFlags & 0x40) == 0x40;

        if (first) {
            if (segment.size < 11) {
                throw new CoreException("Truncated first ODS fragment for object " + objectId);
            }
            if (pendingFrame == null) {
                logger.warn("ODS without PCS at offset "
                        + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8) + " -> ignored\n");
                return;
            }
            int declaredDataLength = ((buffer.getByte(index + 4) & 0xff) << 16)
                    | ((buffer.getByte(index + 5) & 0xff) << 8)
                    | (buffer.getByte(index + 6) & 0xff);
            int width = buffer.getWord(index + 7);
            int height = buffer.getWord(index + 9);
            if (width > pendingFrame.getWidth() || height > pendingFrame.getHeight()) {
                throw new CoreException("Invalid image size " + width + "x" + height
                        + " for object " + objectId);
            }
            ImageObjectFragment fragment = new ImageObjectFragment(index + 11, segment.size - 11);
            // object_data_length includes the 4 size bytes, fragment sizes cover RLE data only
            PGSObject object = new PGSObject(objectId, objectVersion, width, height,
                    declaredDataLength - 4, fragment, last);
            // a new version replaces the previous definition; existing frames keep their own reference
            objectDefinitions.put(objectId, object);
        } else {
            PGSObject object = objectDefinitions.get(objectId);
            if (object == null || object.isComplete()) {
                throw new CoreException("ODS continuation fragment without a started object " + objectId
                        + " at offset " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
            }
            ImageObjectFragment fragment = new ImageObjectFragment(index + 4, segment.size - 4);
            object.addFragment(fragment, last);
        }

        odsInDisplaySet = true;
        logger.trace("ODS offset: " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8)
                + ", id: " + objectId + ", version: " + objectVersion
                + ", seq: " + (first ? "first" : "") + ((first && last) ? "/" : "") + (last ? "last" : "")
                + ", complete: " + objectDefinitions.get(objectId).isComplete() + "\n");
    }

    private void parsePDS(Segment segment) throws FileBufferException, CoreException {
        int index = segment.offset;
        if (segment.size < 2) {
            throw new CoreException("Illegal PDS segment size " + segment.size);
        }
        int paletteId = buffer.getByte(index);
        int paletteVersion = buffer.getByte(index + 1);
        if (paletteId > 7) {
            throw new CoreException("Illegal palette id " + paletteId + " at offset "
                    + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
        }
        PaletteInfo paletteInfo = new PaletteInfo(index + 2, (segment.size - 2) / 5);
        epochPalettes.get(paletteId).add(paletteInfo);
        if (pendingFrame != null) {
            pendingFrame.getPalettes().get(paletteId).add(paletteInfo);
        }
        logger.trace("PDS offset: " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8)
                + ", id: " + paletteId + ", version: " + paletteVersion
                + ", entries: " + paletteInfo.getPaletteSize() + "\n");
    }

    private void commitDisplaySet(Segment segment) throws CoreException {
        logger.trace("END offset: " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8) + "\n");
        if (pendingFrame == null) {
            logger.warn("END without PCS at offset "
                    + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8) + "\n");
            return;
        }

        SubPictureBD frame = pendingFrame;
        pendingFrame = null;

        if (frame.getCompositionRefs().isEmpty()) {
            // a PCS without objects ends the current caption
            if (lastFrame != null) {
                lastFrame.setEndTime(frame.getStartTime());
            }
            lastFrame = null;
            return;
        }

        List<ResolvedComposition> resolved = resolveCompositions(frame, segment);
        if (resolved.isEmpty()) {
            // none of the references has any visible geometry -> no caption frame
            logger.trace("Display set without geometrically visible objects -> ignored\n");
            return;
        }

        frame.getResolvedCompositions().addAll(resolved);
        Rect bounds = calculateBounds(resolved);
        frame.setFrameBounds(bounds.x, bounds.y, bounds.width, bounds.height);

        boolean forced = false;
        for (ResolvedComposition composition : resolved) {
            forced |= composition.isForced();
        }
        frame.setForced(forced);

        if (lastFrame == null || odsInDisplaySet || isDifferentInterval(lastFrame, frame)) {
            if (lastFrame != null) {
                lastFrame.setEndTime(frame.getStartTime());
            }
            subPictures.add(frame);
            lastFrame = frame;
            logger.info("#< " + subPictures.size() + " (" + ptsToTimeStr(frame.getStartTime()) + ")\n");
        } else {
            // unchanged composition without new data: extend the existing interval
            lastFrame.setEndTime(frame.getStartTime());
        }
    }

    /**
     * Resolve the PCS references against the epoch object cache and clip the
     * cropped object rectangles against the referenced windows and the canvas.
     */
    private List<ResolvedComposition> resolveCompositions(SubPictureBD frame, Segment segment)
            throws CoreException {
        List<ResolvedComposition> resolved = new ArrayList<ResolvedComposition>();
        for (CompositionObjectRef ref : frame.getCompositionRefs()) {
            PGSObject object = objectDefinitions.get(ref.getObjectId());
            // every referenced object has to be complete - a new version must
            // not be backed by the previous version's pixels
            if (object == null || !object.isComplete()) {
                throw new CoreException("PCS references incomplete object " + ref.getObjectId()
                        + " at offset " + ToolBox.toHexLeftZeroPadded(segment.offset - 13, 8));
            }

            int cropOriginX = ref.isCropped() ? ref.getCropX() : 0;
            int cropOriginY = ref.isCropped() ? ref.getCropY() : 0;
            int width = ref.isCropped() ? ref.getCropWidth() : object.getWidth();
            int height = ref.isCropped() ? ref.getCropHeight() : object.getHeight();

            // clamp the crop rectangle to the object's pixel bounds
            Rect source = new Rect(cropOriginX, cropOriginY, width, height);
            intersect(source, 0, 0, object.getWidth(), object.getHeight());
            if (source.isEmpty()) {
                continue;
            }
            // the reference x/y is the position of the crop origin
            int destinationOriginX = ref.getX() + (source.x - cropOriginX);
            int destinationOriginY = ref.getY() + (source.y - cropOriginY);

            Rect rect = new Rect(destinationOriginX, destinationOriginY, source.width, source.height);
            intersect(rect, 0, 0, frame.getWidth(), frame.getHeight());
            WindowDefinition window = frame.getWindows().get(ref.getWindowId());
            if (window != null) {
                intersect(rect, window.getX(), window.getY(), window.getWidth(), window.getHeight());
            }
            if (rect.isEmpty()) {
                continue;
            }
            // keep the source origin consistent with the clipped edges
            int sourceX = source.x + (rect.x - destinationOriginX);
            int sourceY = source.y + (rect.y - destinationOriginY);

            resolved.add(new ResolvedComposition(object, sourceX, sourceY,
                    rect.x, rect.y, rect.width, rect.height, ref.isForced()));
        }
        return resolved;
    }

    private Rect calculateBounds(List<ResolvedComposition> compositions) {
        Rect union = null;
        for (ResolvedComposition composition : compositions) {
            if (union == null) {
                union = new Rect(composition.getDestinationX(), composition.getDestinationY(),
                        composition.getWidth(), composition.getHeight());
            } else {
                int xMax = Math.max(union.x + union.width, composition.getDestinationX() + composition.getWidth());
                int yMax = Math.max(union.y + union.height, composition.getDestinationY() + composition.getHeight());
                union.x = Math.min(union.x, composition.getDestinationX());
                union.y = Math.min(union.y, composition.getDestinationY());
                union.width = xMax - union.x;
                union.height = yMax - union.y;
            }
        }
        return union;
    }

    private void intersect(Rect rect, int x, int y, int width, int height) {
        int x1 = Math.max(rect.x, x);
        int y1 = Math.max(rect.y, y);
        int x2 = Math.min(rect.x + rect.width, x + width);
        int y2 = Math.min(rect.y + rect.height, y + height);
        rect.x = x1;
        rect.y = y1;
        rect.width = Math.max(0, x2 - x1);
        rect.height = Math.max(0, y2 - y1);
    }

    private boolean isDifferentInterval(SubPictureBD previous, SubPictureBD current) {
        if (previous.getPaletteId() != current.getPaletteId()) {
            return true;
        }
        if (!previous.getCompositionRefs().equals(current.getCompositionRefs())) {
            return true;
        }
        if (!previous.getWindows().equals(current.getWindows())) {
            return true;
        }
        if (!previous.getPalettes().equals(current.getPalettes())) {
            return true;
        }
        return false;
    }

    private void countForcedFrames() {
        forcedFrameCount = 0;
        for (SubPictureBD p : subPictures) {
            if (p.isForced()) {
                forcedFrameCount++;
            }
        }
        logger.info("\nDetected " + forcedFrameCount + " forced captions.\n");
    }

    public FileBuffer getBuffer() {
        return buffer;
    }

    public List<SubPictureBD> getSubPictures() {
        return subPictures;
    }

    public int getForcedFrameCount() {
        return forcedFrameCount;
    }
}
