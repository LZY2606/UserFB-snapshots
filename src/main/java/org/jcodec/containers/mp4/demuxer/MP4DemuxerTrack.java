package org.jcodec.containers.mp4.demuxer;

import static org.jcodec.containers.mp4.QTTimeUtil.mediaToEdited;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Arrays;

import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.containers.mp4.MP4Packet;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.CompositionOffsetsBox;
import org.jcodec.containers.mp4.boxes.CompositionOffsetsBox.Entry;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleSizesBox;
import org.jcodec.containers.mp4.boxes.SyncSamplesBox;
import org.jcodec.containers.mp4.boxes.TrakBox;

/**
 * This class is part of JCodec ( www.jcodec.org ) This software is distributed
 * under FreeBSD License
 *
 * Regular MP4 track containing frames
 *
 * @author The JCodec project
 *
 */
public class MP4DemuxerTrack extends AbstractMP4DemuxerTrack {

    private int[] sizes;

    private long offInChunk;

    private int noInChunk;

    private int[] syncSamples;
    private int[] partialSync;
    private int ssOff;
    private int psOff;

    private Entry[] compOffsets;
    private int cttsInd;
    private int cttsSubInd;

    private SeekableByteChannel input;

    private MovieBox movie;

    /**
     * Read pointer of a frames track: the shared sample table cursors plus the
     * position inside the current chunk.
     */
    static final class FrameCursor extends Cursor {
        final long offInChunk;
        final int noInChunk;
        final int ssOff;
        final int psOff;
        final int cttsInd;
        final int cttsSubInd;

        FrameCursor(int sttsInd, int sttsSubInd, int stcoInd, int stscInd, long pts, long curFrame, long offInChunk,
                int noInChunk, int ssOff, int psOff, int cttsInd, int cttsSubInd) {
            super(sttsInd, sttsSubInd, stcoInd, stscInd, pts, curFrame);
            this.offInChunk = offInChunk;
            this.noInChunk = noInChunk;
            this.ssOff = ssOff;
            this.psOff = psOff;
            this.cttsInd = cttsInd;
            this.cttsSubInd = cttsSubInd;
        }
    }

    // Seek indexes, built once on demand. All O(sample count + chunk count).
    private long[] samplePts;
    private long[] sampleFileOff;
    private long[] chunkFirstFrame;
    private int[] chunkStscInd;
    private int[] sttsCumCount;
    private boolean seekIndexBuilt;

    public MP4DemuxerTrack(MovieBox mov, TrakBox trak, SeekableByteChannel input) {
        super(trak);
        this.input = input;
        this.movie = mov;
        SampleSizesBox stsz = NodeBox.findFirstPath(trak, SampleSizesBox.class, Box.path("mdia.minf.stbl.stsz"));
        SyncSamplesBox stss = NodeBox.findFirstPath(trak, SyncSamplesBox.class, Box.path("mdia.minf.stbl.stss"));
        SyncSamplesBox stps = NodeBox.findFirstPath(trak, SyncSamplesBox.class, Box.path("mdia.minf.stbl.stps"));
        CompositionOffsetsBox ctts = NodeBox.findFirstPath(trak, CompositionOffsetsBox.class, Box.path("mdia.minf.stbl.ctts"));
        compOffsets = ctts == null ? null : ctts.getEntries();
        if (stss != null) {
            syncSamples = stss.getSyncSamples();
        }
        if (stps != null) {
            partialSync = stps.getSyncSamples();
        }

        if (stsz.getDefaultSize() != 0) {
            sizes = new int[stsz.getCount()];
            Arrays.fill(sizes, stsz.getDefaultSize());
        } else
            sizes = stsz.getSizes();
    }

    @Override
    public synchronized MP4Packet nextFrame() throws IOException {
        if (curFrame >= sizes.length)
            return null;
        int size = sizes[(int) curFrame];

        return getNextFrame(ByteBuffer.allocate(size));
    }

    @Override
    public synchronized MP4Packet getNextFrame(ByteBuffer storage) throws IOException {
        MP4Packet raw = readRawFrame(storage);
        if (raw == null)
            return null;
        long editedPts = mediaToEdited(box, raw.getMediaPts(), movie.getTimescale());
        return new MP4Packet(raw.data, editedPts, timescale, raw.duration, raw.frameNo, raw.frameType, null, 0,
                raw.getMediaPts(), raw.getEntryNo(), raw.getFileOff(), raw.getSize(), raw.isPsync());
    }

    /**
     * Reads the frame at the current cursor using the real payload loading
     * logic, stamped with the native media timing (no edit remapping). On any
     * read failure the cursor is rolled back to its pre-read state, so retrying
     * with a recovered channel yields the very same frame.
     */
    synchronized MP4Packet readRawFrame(ByteBuffer storage) throws IOException {
        if (curFrame >= sizes.length)
            return null;
        int size = sizes[(int) curFrame];

        if (storage != null && storage.remaining() < size) {
            throw new IllegalArgumentException("Buffer size is not enough to fit a packet");
        }

        Cursor saved = prepareCursor();
        int storagePos = storage == null ? 0 : storage.position();
        int storageLimit = storage == null ? 0 : storage.limit();
        long pktPos = chunkOffsets[Math.min(chunkOffsets.length - 1, stcoInd)] + offInChunk;

        ByteBuffer result;
        try {
            result = readPacketData(input, storage, pktPos, size);
        } catch (IOException | RuntimeException e) {
            restoreCursor(saved);
            if (storage != null) {
                storage.limit(storageLimit);
                storage.position(storagePos);
            }
            throw e;
        }

        if (result == null || result.remaining() < size) {
            restoreCursor(saved);
            if (storage != null) {
                storage.limit(storageLimit);
                storage.position(storagePos);
            }
            throw new IOException("Short read of frame " + curFrame + " at offset " + pktPos);
        }

        int duration = timeToSamples[sttsInd].getSampleDuration();

        boolean sync = syncSamples == null;

        if (syncSamples != null && ssOff < syncSamples.length && (curFrame + 1) == syncSamples[ssOff]) {
            sync = true;
            ssOff++;
        }

        boolean psync = false;
        if (partialSync != null && psOff < partialSync.length && (curFrame + 1) == partialSync[psOff]) {
            psync = true;
            psOff++;
        }

        long realPts = pts;
        if (compOffsets != null) {
            realPts = pts + compOffsets[cttsInd].getOffset();
            cttsSubInd++;
            if (cttsInd < compOffsets.length - 1 && cttsSubInd == compOffsets[cttsInd].getCount()) {
                cttsInd++;
                cttsSubInd = 0;
            }
        }

        ByteBuffer data = result == null ? null : convertPacket(result);
        int entryNo = sampleToChunks[stscInd].getEntry() - 1;
        MP4Packet pkt = new MP4Packet(data, realPts, timescale, duration, curFrame,
                sync ? FrameType.KEY : FrameType.INTER, null, 0, realPts, entryNo, pktPos, size, psync);

        offInChunk += size;

        curFrame++;
        noInChunk++;
        if (noInChunk >= sampleToChunks[stscInd].getCount()) {
            noInChunk = 0;
            offInChunk = 0;

            nextChunk();
        }
        shiftPts(1);

        return pkt;
    }

    @Override
    protected Cursor prepareCursor() {
        return new FrameCursor(sttsInd, sttsSubInd, stcoInd, stscInd, pts, curFrame, offInChunk, noInChunk, ssOff,
                psOff, cttsInd, cttsSubInd);
    }

    @Override
    protected void restoreCursor(Cursor cursor) {
        super.restoreCursor(cursor);
        FrameCursor c = (FrameCursor) cursor;
        offInChunk = c.offInChunk;
        noInChunk = c.noInChunk;
        ssOff = c.ssOff;
        psOff = c.psOff;
        cttsInd = c.cttsInd;
        cttsSubInd = c.cttsSubInd;
    }

    /**
     * Builds cumulative sample/chunk indexes so cursor positioning is a set of
     * binary searches instead of a prefix re-scan. Memory stays linear in the
     * sample and chunk counts.
     */
    synchronized void buildSeekIndex() {
        if (seekIndexBuilt)
            return;
        int n = sizes.length;
        samplePts = new long[n];
        sampleFileOff = new long[n];
        sttsCumCount = new int[timeToSamples.length];
        int stts = 0;
        long cumulativePts = 0;
        int cumulativeCount = 0;
        for (int i = 0; i < timeToSamples.length; i++) {
            cumulativeCount += timeToSamples[i].getSampleCount();
            sttsCumCount[i] = cumulativeCount;
        }
        for (int i = 0; i < n; i++) {
            while (stts < timeToSamples.length - 1 && i >= sttsCumCount[stts])
                stts++;
            samplePts[i] = cumulativePts;
            cumulativePts += timeToSamples[stts].getSampleDuration();
        }

        int chunks = chunkOffsets.length;
        chunkFirstFrame = new long[chunks];
        chunkStscInd = new int[chunks];
        int stsc = 0;
        long firstFrame = 0;
        long byteOff = 0;
        for (int c = 0; c < chunks; c++) {
            if (stsc + 1 < sampleToChunks.length && c + 1 == sampleToChunks[stsc + 1].getFirst())
                stsc++;
            chunkFirstFrame[c] = firstFrame;
            chunkStscInd[c] = stsc;
            int count = sampleToChunks[stsc].getCount();
            long chunkBase = chunkOffsets[c];
            for (int j = 0; j < count; j++) {
                int sample = (int) (firstFrame + j);
                sampleFileOff[sample] = chunkBase + byteOff;
                byteOff += sizes[sample];
            }
            firstFrame += count;
            byteOff = 0;
        }
        seekIndexBuilt = true;
    }

    private void requireSeekIndex() {
        if (!seekIndexBuilt)
            buildSeekIndex();
    }

    /**
     * Computes the read pointer for an arbitrary frame in O(log sample count +
     * log chunk count) without touching the active cursor. Commit it with
     * {@link #commitCursor(Cursor)} once the move is confirmed.
     */
    synchronized FrameCursor cursorAtFrame(long frameNo) {
        requireSeekIndex();
        if (frameNo < 0 || frameNo >= sizes.length)
            throw new IllegalArgumentException("Frame out of range: " + frameNo);
        int sttsI = Arrays.binarySearch(sttsCumCount, (int) frameNo + 1);
        if (sttsI < 0)
            sttsI = -sttsI - 1;
        int prevCount = sttsI == 0 ? 0 : sttsCumCount[sttsI - 1];
        int sttsSub = (int) frameNo - prevCount;

        int chunk = Arrays.binarySearch(chunkFirstFrame, frameNo);
        if (chunk < 0)
            chunk = -chunk - 2;
        int stscI = chunkStscInd[chunk];
        int noIn = (int) (frameNo - chunkFirstFrame[chunk]);
        long off = sampleFileOff[(int) frameNo] - chunkOffsets[chunk];

        int ss = syncSamples == null ? 0 : Arrays.binarySearch(syncSamples, (int) frameNo + 1);
        if (ss < 0)
            ss = -ss - 1;
        int ps = partialSync == null ? 0 : Arrays.binarySearch(partialSync, (int) frameNo + 1);
        if (ps < 0)
            ps = -ps - 1;

        int cttsI = 0;
        int cttsSub = (int) frameNo;
        if (compOffsets != null) {
            while (cttsSub >= compOffsets[cttsI].getCount()) {
                cttsSub -= compOffsets[cttsI].getCount();
                cttsI++;
            }
        }
        return new FrameCursor(sttsI, sttsSub, chunk, stscI, samplePts[(int) frameNo], frameNo, off, noIn, ss, ps,
                cttsI, cttsSub);
    }

    int[] getSampleSizes() {
        return sizes;
    }

    long[] samplePtsArray() {
        requireSeekIndex();
        return samplePts;
    }

    long frameDurationAt(int frame) {
        requireSeekIndex();
        int sttsI = Arrays.binarySearch(sttsCumCount, frame + 1);
        if (sttsI < 0)
            sttsI = -sttsI - 1;
        return timeToSamples[sttsI].getSampleDuration();
    }

    @Override
    public boolean gotoSyncFrame(long frameNo) {
        if (syncSamples == null)
            return gotoFrame(frameNo);
        if (frameNo < 0)
            throw new IllegalArgumentException("negative frame number");
        if (frameNo >= getFrameCount())
            return false;
        if (frameNo == curFrame)
            return true;
        for (int i = 0; i < syncSamples.length; i++) {
            if (syncSamples[i] - 1 > frameNo)
                return gotoFrame(syncSamples[i - 1] - 1);
        }
        return gotoFrame(syncSamples[syncSamples.length - 1] - 1);
    }

    @Override
    protected void seekPointer(long frameNo) {
        if (compOffsets != null) {
            cttsSubInd = (int) frameNo;
            cttsInd = 0;
            while (cttsSubInd >= compOffsets[cttsInd].getCount()) {
                cttsSubInd -= compOffsets[cttsInd].getCount();
                cttsInd++;
            }
        }

        curFrame = (int) frameNo;
        stcoInd = 0;
        stscInd = 0;
        noInChunk = (int) frameNo;
        offInChunk = 0;

        while (noInChunk >= sampleToChunks[stscInd].getCount()) {
            noInChunk -= sampleToChunks[stscInd].getCount();

            nextChunk();
        }

        for (int i = 0; i < noInChunk; i++) {
            offInChunk += sizes[(int) frameNo - noInChunk + i];
        }

        if (syncSamples != null)
            for (ssOff = 0; ssOff < syncSamples.length && syncSamples[ssOff] < curFrame + 1; ssOff++)
                ;

        if (partialSync != null)
            for (psOff = 0; psOff < partialSync.length && partialSync[psOff] < curFrame + 1; psOff++)
                ;

    }

    @Override
    public long getFrameCount() {
        return sizes.length;
    }
}
