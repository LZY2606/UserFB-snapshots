/*
 * Copyright 2014 Volker Oth (0xdeadbeef) / Miklos Juhasz (mjuhasz)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package bdsup2sub.supstream.bd;

import bdsup2sub.bitmap.Bitmap;
import bdsup2sub.bitmap.Palette;
import bdsup2sub.core.*;
import bdsup2sub.supstream.ImageObjectFragment;
import bdsup2sub.supstream.PaletteInfo;
import bdsup2sub.supstream.SubPicture;
import bdsup2sub.supstream.SubtitleStream;
import bdsup2sub.tools.FileBuffer;
import bdsup2sub.tools.FileBufferException;
import bdsup2sub.utils.ToolBox;

import java.awt.image.BufferedImage;
import java.util.ArrayList;
import java.util.List;

/**
 * Reading and writing of Blu-Ray captions demuxed from M2TS transport streams (BD-SUP).
 * A caption may consist of up to two composition objects; decode/getBitmap/getPalette
 * and getSubPicture always operate on the fully composed caption.
 */
public class SupBD implements SubtitleStream {

    private static final Configuration configuration = Configuration.getInstance();
    private static final Logger logger = Logger.getInstance();

    private FileBuffer buffer;
    private List<SubPictureBD> subPictures = new ArrayList<SubPictureBD>();
    private int forcedFrameCount;

    /** color palette of the last decoded caption  */
    private Palette palette;
    /** bitmap of the last decoded caption  */
    private Bitmap bitmap;
    /** index of dominant color for the current caption  */
    private int primaryColorIndex;

    public SupBD(String filename) throws CoreException {
        SupBDParser parser = new SupBDParser(filename);
        buffer = parser.getBuffer();
        subPictures = parser.getSubPictures();
        forcedFrameCount = parser.getForcedFrameCount();
    }

    /**
     * Decode the RLE stream of one object definition.
     * @param object object definition containing the RLE fragments
     * @param transparentColorIndex index used to initialize the bitmap
     * @return bitmap of the decoded object
     * @throws CoreException
     */
    private Bitmap decodeObject(PGSObject object, int transparentColorIndex) throws CoreException {
        int width = object.getWidth();
        int height = object.getHeight();
        ImageObjectFragment firstFragment = object.getFragments().get(0);
        long startOfs = firstFragment.getImageBufferOfs();

        Bitmap bm = new Bitmap(width, height, (byte) transparentColorIndex);

        int b;
        int index = 0;
        int ofs = 0;
        int size;
        int xpos = 0;

        try {
            // just for multi-packet support, copy all of the image data in one common buffer
            byte[] rleBuffer = new byte[object.getBufferSize()];
            index = 0;
            for (ImageObjectFragment fragment : object.getFragments()) {
                // copy data of all packets to one common buffer
                for (int i = 0; i < fragment.getImagePacketSize(); i++) {
                    rleBuffer[index + i] = (byte) buffer.getByte(fragment.getImageBufferOfs() + i);
                }
                index += fragment.getImagePacketSize();
            }

            index = 0;
            do {
                b = rleBuffer[index++] & 0xff;
                if (b == 0) {
                    b = rleBuffer[index++] & 0xff;
                    if (b == 0) {
                        // next line
                        ofs = (ofs / width) * width;
                        if (xpos < width) {
                            ofs += width;
                        }
                        xpos = 0;
                    } else {
                        if ((b & 0xC0) == 0x40) {
                            // 00 4x xx -> xxx zeroes
                            size = ((b - 0x40) << 8) + (rleBuffer[index++] & 0xff);
                            for (int i = 0; i < size; i++) {
                                bm.getInternalBuffer()[ofs++] = 0;
                            }
                            xpos += size;
                        } else if ((b & 0xC0) == 0x80) {
                            // 00 8x yy -> x times value y
                            size = (b - 0x80);
                            b = rleBuffer[index++] & 0xff;
                            for (int i = 0; i < size; i++) {
                                bm.getInternalBuffer()[ofs++] = (byte) b;
                            }
                            xpos += size;
                        } else if ((b & 0xC0) != 0) {
                            // 00 cx yy zz -> xyy times value z
                            size = ((b - 0xC0) << 8) + (rleBuffer[index++] & 0xff);
                            b = rleBuffer[index++] & 0xff;
                            for (int i = 0; i < size; i++) {
                                bm.getInternalBuffer()[ofs++] = (byte) b;
                            }
                            xpos += size;
                        } else {
                            // 00 xx -> xx times 0
                            for (int i = 0; i < b; i++) {
                                bm.getInternalBuffer()[ofs++] = 0;
                            }
                            xpos += b;
                        }
                    }
                } else {
                    bm.getInternalBuffer()[ofs++] = (byte) b;
                    xpos++;
                }
            } while (index < rleBuffer.length);

            return bm;
        } catch (FileBufferException ex) {
            throw new CoreException(ex.getMessage());
        } catch (ArrayIndexOutOfBoundsException ex) {
            logger.warn("Problems during RLE decoding of picture OBJ at offset "
                    + ToolBox.toHexLeftZeroPadded(startOfs + index, 8) + "\n");
            return bm;
        }
    }

    /**
     * Decode all composition objects of a caption and composite them in PCS
     * order. Palette indices with an alpha of zero let the lower layers show
     * through, all other indices overwrite the previous pixels. The output
     * bitmap covers the bounding box of the cropped object rectangles.
     * @param pic SubPicture object containing info about the caption
     * @return composed bitmap of the caption
     * @throws CoreException
     */
    private Bitmap decodeImage(SubPictureBD pic, int transparentColorIndex) throws CoreException {
        Bitmap composed = new Bitmap(pic.getImageWidth(), pic.getImageHeight(), (byte) transparentColorIndex);
        byte[] alpha = palette.getAlpha();
        byte[] target = composed.getInternalBuffer();
        int frameWidth = composed.getWidth();

        for (ResolvedComposition composition : pic.getResolvedCompositions()) {
            PGSObject object = composition.getObject();
            if (object.getWidth() > pic.getWidth() || object.getHeight() > pic.getHeight()) {
                throw new CoreException("Subpicture too large: " + object.getWidth() + "x" + object.getHeight()
                        + " at offset " + ToolBox.toHexLeftZeroPadded(object.getFragments().get(0).getImageBufferOfs(), 8));
            }
            Bitmap objectBitmap = decodeObject(object, transparentColorIndex);
            byte[] source = objectBitmap.getInternalBuffer();
            int objectWidth = objectBitmap.getWidth();
            int baseX = composition.getDestinationX() - pic.getXOffset();
            int baseY = composition.getDestinationY() - pic.getYOffset();

            for (int y = 0; y < composition.getHeight(); y++) {
                int sourceRowOfs = (composition.getSourceY() + y) * objectWidth + composition.getSourceX();
                int targetRowOfs = (baseY + y) * frameWidth + baseX;
                for (int x = 0; x < composition.getWidth(); x++) {
                    int colorIndex = source[sourceRowOfs + x] & 0xff;
                    if ((alpha[colorIndex] & 0xff) == 0) {
                        // fully transparent index: let the lower layer show through
                        continue;
                    }
                    target[targetRowOfs + x] = (byte) colorIndex;
                }
            }
        }
        return composed;
    }

    /**
     * decode palette from the input stream
     * @param pic SubPicture object containing info about the current frame
     * @return
     * @throws CoreException
     */
    private Palette decodePalette(SubPictureBD pic) throws CoreException {
        boolean fadeOut = false;
        int paletteIndex;
        List<PaletteInfo> paletteInfos = pic.getPalettes().get(pic.getPaletteId());
        if (paletteInfos == null) {
            throw new CoreException("Palette ID out of bounds.");
        }

        Palette palette = new Palette(256, Core.usesBT601());
        // by definition, index 0xff is always completely transparent
        // also all entries must be fully transparent after initialization

        try {
            for (PaletteInfo paletteInfo : paletteInfos) {
                int index = paletteInfo.getPaletteOffset();
                for (int i = 0; i < paletteInfo.getPaletteSize(); i++) {
                    // each palette entry consists of 5 bytes
                    paletteIndex = buffer.getByte(index);
                    int y = buffer.getByte(++index);
                    int cr, cb;
                    if (configuration.isSwapCrCb()) {
                        cb = buffer.getByte(++index);
                        cr = buffer.getByte(++index);
                    } else {
                        cr = buffer.getByte(++index);
                        cb = buffer.getByte(++index);
                    }
                    int alpha = buffer.getByte(++index);

                    int alphaOld = palette.getAlpha(paletteIndex);
                    // avoid fading out
                    if (alpha >= alphaOld) {
                        if (alpha < configuration.getAlphaCrop()) {// to not mess with scaling algorithms, make transparent color black
                            y = 16;
                            cr = 128;
                            cb = 128;
                        }
                        palette.setAlpha(paletteIndex, alpha);
                    } else {
                        fadeOut = true;
                    }

                    palette.setYCbCr(paletteIndex, y, cb, cr);
                    index++;
                }
            }
            if (fadeOut) {
                logger.warn("fade out detected -> patched palette\n");
            }
            return palette;
        } catch (FileBufferException ex) {
            throw new CoreException(ex.getMessage());
        }
    }

    /**
     * decode given picture
     * @param pic SubPicture object containing info about caption
     * @throws CoreException
     */
    private void decode(SubPictureBD pic) throws CoreException {
        palette = decodePalette(pic);
        bitmap = decodeImage(pic, palette.getIndexOfMostTransparentPaletteEntry());
        primaryColorIndex = bitmap.getPrimaryColorIndex(palette.getAlpha(), configuration.getAlphaThreshold(), palette.getY());
    }

    /* (non-Javadoc)
     * @see SubtitleStream#decode(int)
     */
    public void decode(int index) throws CoreException {
        if (index < subPictures.size()) {
            decode(subPictures.get(index));
        } else {
            throw new CoreException("Index " + index + " out of bounds\n");
        }
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getPalette()
     */
    public Palette getPalette() {
        return palette;
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getBitmap()
     */
    public Bitmap getBitmap() {
        return bitmap;
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getImage()
     */
    public BufferedImage getImage() {
        return bitmap.getImage(palette.getColorModel());
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getImage(Bitmap)
     */
    public BufferedImage getImage(Bitmap bm) {
        return bm.getImage(palette.getColorModel());
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getPrimaryColorIndex()
     */
    public int getPrimaryColorIndex() {
        return primaryColorIndex;
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getSubPicture(int)
     */
    public SubPicture getSubPicture(int index) {
        return subPictures.get(index);
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getNumFrames()
     */
    public int getFrameCount() {
        return subPictures.size();
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getForcedFrameCount()
     */
    public int getForcedFrameCount() {
        return forcedFrameCount;
    }

    /* (non-Javadoc)
     * @see SubtitleStream#close()
     */
    public void close() {
        if (buffer != null) {
            buffer.close();
        }
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getEndTime(int)
     */
    public long getEndTime(int index) {
        return subPictures.get(index).getEndTime();
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getStartTime(int)
     */
    public long getStartTime(int index) {
        return subPictures.get(index).getStartTime();
    }

    /* (non-Javadoc)
     * @see SubtitleStream#isForced(int)
     */
    public boolean isForced(int index) {
        return subPictures.get(index).isForced();
    }

    /* (non-Javadoc)
     * @see SubtitleStream#getStartOffset(int)
     */
    public long getStartOffset(int index) {
        SubPictureBD pic = subPictures.get(index);
        return pic.getResolvedCompositions().get(0).getObject().getFragments().get(0).getImageBufferOfs();
    }

    /**
     * Get frame rate for given caption
     * @param index index of caption
     * @return frame rate
     */
    public double getFps(int index) {
        return Framerate.valueForId(subPictures.get(index).getType());
    }
}
