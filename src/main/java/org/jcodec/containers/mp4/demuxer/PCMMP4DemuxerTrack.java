package org.jcodec.containers.mp4.demuxer;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Arrays;

import org.jcodec.common.AudioCodecMeta;
import org.jcodec.common.Codec;
import org.jcodec.common.DemuxerTrackMeta;
import org.jcodec.common.TrackType;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.containers.mp4.MP4Packet;
import org.jcodec.containers.mp4.QTTimeUtil;
import org.jcodec.containers.mp4.boxes.AudioSampleEntry;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleEntry;
import org.jcodec.containers.mp4.boxes.SampleSizesBox;
import org.jcodec.containers.mp4.boxes.TrakBox;

/**
 * This class is part of JCodec ( www.jcodec.org ) This software is distributed
 * under FreeBSD License
 *
 * Specialized demuxer track for PCM audio samples
 *
 * Always reads one chunk of frames at a time, except for after seek. After seek
 * the beginning of chunk before the seek point is not read effectivaly reading
 * PCM frame from exactly the frame seek was performed to.
 *
 * Packet size depends on underlying container PCM chunk sizes.
 *
 * @author The JCodec project
 *
 */
public class PCMMP4DemuxerTrack extends AbstractMP4DemuxerTrack {

    private int defaultSampleSize;

    private int posShift;

    protected int totalFrames;

    private SeekableByteChannel input;

    private MovieBox movie;

    /**
     * Read pointer of a PCM track: the shared sample table cursors plus the
     * byte offset inside the current chunk left over from a frame-granular
     * positioning.
     */
    static final class PCMCursor extends Cursor {
        final int posShift;

        PCMCursor(int sttsInd, int sttsSubInd, int stcoInd, int stscInd, long pts, long curFrame, int posShift) {
            super(sttsInd, sttsSubInd, stcoInd, stscInd, pts, curFrame);
            this.posShift = posShift;
        }
    }

    // Seek indexes, built once on demand. All O(frame count + chunk count).
    private long[] chunkFirstFrame;
    private int[] chunkStscInd;
    private int[] sttsCumCount;
    private long[] sttsCumPts;
    private boolean seekIndexBuilt;

    public PCMMP4DemuxerTrack(MovieBox movie, TrakBox trak, SeekableByteChannel input) {
        super(trak);

        this.movie = movie;
        this.input = input;
        SampleSizesBox stsz = NodeBox.findFirstPath(trak, SampleSizesBox.class, Box.path("mdia.minf.stbl.stsz"));
        defaultSampleSize = stsz.getDefaultSize();

        int chunks = 0;
        for (int i = 1; i < sampleToChunks.length; i++) {
            int ch = (int) (sampleToChunks[i].getFirst() - sampleToChunks[i - 1].getFirst());
            totalFrames += ch * sampleToChunks[i - 1].getCount();
            chunks += ch;
        }
        totalFrames += sampleToChunks[sampleToChunks.length - 1].getCount() * (chunkOffsets.length - chunks);
    }

    @Override
    public Packet nextFrame() throws IOException {
        int frameSize = getFrameSize();
        int chSize = sampleToChunks[stscInd].getCount() * frameSize - posShift;

        return getNextFrame(ByteBuffer.allocate(chSize));
    }

    @Override
    public synchronized MP4Packet getNextFrame(ByteBuffer buffer) throws IOException {
        if (stcoInd >= chunkOffsets.length)
            return null;
        MP4Packet raw = readRawFrames(buffer, sampleToChunks[stscInd].getCount());
        if (raw == null)
            return null;
        long editedPts = QTTimeUtil.mediaToEdited(box, raw.getMediaPts(), movie.getTimescale());
        return new MP4Packet(raw.data, editedPts, timescale, raw.duration, raw.frameNo, FrameType.KEY, null, 0,
                raw.getMediaPts(), raw.getEntryNo(), raw.getFileOff(), raw.getSize(), true);
    }

    /**
     * Reads up to {@code frameCount} whole PCM frames starting at the current
     * cursor, never crossing a physical chunk. The bytes come straight from the
     * underlying chunk through the real payload loading logic. On any read
     * failure the cursor is rolled back, so a retry returns the same data.
     *
     * @return the packet, or {@code null} when all media has been consumed
     */
    synchronized MP4Packet readRawFrames(ByteBuffer buffer, int frameCount) throws IOException {
        if (stcoInd >= chunkOffsets.length)
            return null;
        buildSeekIndex();
        int frameSize = getFrameSize();

        int se = sampleToChunks[stscInd].getEntry();
        int chunkFrames = sampleToChunks[stscInd].getCount();
        long chunkStartFrame = chunkFirstFrame[stcoInd];
        int frameInChunk = (int) (curFrame - chunkStartFrame);
        int available = chunkFrames - frameInChunk;
        int doneFrames = Math.min(frameCount, available);
        if (doneFrames <= 0)
            return null;

        int byteShift = frameInChunk * frameSize;
        long pktOff = chunkOffsets[stcoInd] + byteShift;
        int pktSize = doneFrames * frameSize;

        if (buffer != null && buffer.remaining() < pktSize) {
            throw new IllegalArgumentException("Buffer size is not enough to fit a packet");
        }

        Cursor saved = prepareCursor();
        int storagePos = buffer == null ? 0 : buffer.position();
        int storageLimit = buffer == null ? 0 : buffer.limit();
        ByteBuffer result;
        try {
            result = readPacketData(input, buffer, pktOff, pktSize);
        } catch (IOException | RuntimeException e) {
            restoreCursor(saved);
            if (buffer != null) {
                buffer.limit(storageLimit);
                buffer.position(storagePos);
            }
            throw e;
        }
        if (result == null || result.remaining() < pktSize) {
            restoreCursor(saved);
            if (buffer != null) {
                buffer.limit(storageLimit);
                buffer.position(storagePos);
            }
            throw new IOException("Short read of PCM frames at offset " + pktOff);
        }

        long ptsRem = pts;
        shiftPts(doneFrames);

        MP4Packet pkt = new MP4Packet(result, ptsRem, timescale, doneFrames, curFrame, FrameType.KEY, null, 0, ptsRem,
                se - 1, pktOff, pktSize, true);

        curFrame += doneFrames;

        if (doneFrames == available) {
            ++stcoInd;
            if (stscInd < sampleToChunks.length - 1 && (stcoInd + 1) == sampleToChunks[stscInd + 1].getFirst())
                stscInd++;
            posShift = 0;
        } else {
            posShift = (frameInChunk + doneFrames) * frameSize;
        }

        return pkt;
    }

    @Override
    protected Cursor prepareCursor() {
        return new PCMCursor(sttsInd, sttsSubInd, stcoInd, stscInd, pts, curFrame, posShift);
    }

    @Override
    protected void restoreCursor(Cursor cursor) {
        super.restoreCursor(cursor);
        posShift = ((PCMCursor) cursor).posShift;
    }

    /**
     * Builds cumulative frame/chunk indexes so positioning is a binary search
     * instead of a walk over the stsc/stts prefix.
     */
    synchronized void buildSeekIndex() {
        if (seekIndexBuilt)
            return;
        int chunks = chunkOffsets.length;
        chunkFirstFrame = new long[chunks];
        chunkStscInd = new int[chunks];
        int stsc = 0;
        long firstFrame = 0;
        for (int c = 0; c < chunks; c++) {
            if (stsc + 1 < sampleToChunks.length && c + 1 == sampleToChunks[stsc + 1].getFirst())
                stsc++;
            chunkFirstFrame[c] = firstFrame;
            chunkStscInd[c] = stsc;
            firstFrame += sampleToChunks[stsc].getCount();
        }
        sttsCumCount = new int[timeToSamples.length];
        sttsCumPts = new long[timeToSamples.length];
        int cumulative = 0;
        long cumulativePts = 0;
        for (int i = 0; i < timeToSamples.length; i++) {
            sttsCumPts[i] = cumulativePts;
            cumulativePts += timeToSamples[i].getSegmentDuration();
            cumulative += timeToSamples[i].getSampleCount();
            sttsCumCount[i] = cumulative;
        }
        seekIndexBuilt = true;
    }

    /**
     * Media presentation time of an arbitrary frame, resolved in O(log stts).
     */
    long ptsAtFrame(long frameNo) {
        buildSeekIndex();
        int sttsI = Arrays.binarySearch(sttsCumCount, (int) frameNo + 1);
        if (sttsI < 0)
            sttsI = -sttsI - 1;
        int prevCount = sttsI == 0 ? 0 : sttsCumCount[sttsI - 1];
        return sttsCumPts[sttsI] + (frameNo - prevCount) * timeToSamples[sttsI].getSampleDuration();
    }

    /**
     * Computes the read pointer for an arbitrary media frame in O(log frame
     * count + log chunk count) without touching the active cursor.
     */
    synchronized PCMCursor cursorAtFrame(long frameNo) {
        buildSeekIndex();
        if (frameNo < 0 || frameNo >= totalFrames)
            throw new IllegalArgumentException("Frame out of range: " + frameNo);
        int chunk = Arrays.binarySearch(chunkFirstFrame, frameNo);
        if (chunk < 0)
            chunk = -chunk - 2;
        int stscI = chunkStscInd[chunk];
        int frameInChunk = (int) (frameNo - chunkFirstFrame[chunk]);

        int sttsI = Arrays.binarySearch(sttsCumCount, (int) frameNo + 1);
        if (sttsI < 0)
            sttsI = -sttsI - 1;
        int prevCount = sttsI == 0 ? 0 : sttsCumCount[sttsI - 1];
        int sttsSub = (int) frameNo - prevCount;

        return new PCMCursor(sttsI, sttsSub, chunk, stscI, ptsAtFrame(frameNo), frameNo,
                frameInChunk * getFrameSize());
    }

    int getTotalMediaFrames() {
        return totalFrames;
    }

    int getPcmFrameSize() {
        return getFrameSize();
    }

    long[] chunkFirstFrameArray() {
        buildSeekIndex();
        return chunkFirstFrame;
    }

    @Override
    public boolean gotoSyncFrame(long frameNo) {
        return gotoFrame(frameNo);
    }

    public int getFrameSize() {
        SampleEntry entry = sampleEntries[sampleToChunks[stscInd].getEntry() - 1];
        if (entry instanceof AudioSampleEntry && defaultSampleSize == 0) {
            return ((AudioSampleEntry) entry).calcFrameSize();
        } else {
            return defaultSampleSize;
        }
    }

    @Override
    protected void seekPointer(long frameNo) {
        for (stcoInd = 0, stscInd = 0, curFrame = 0;;) {
            long nextFrame = curFrame + sampleToChunks[stscInd].getCount();
            if (nextFrame > frameNo)
                break;
            curFrame = nextFrame;
            nextChunk();
        }
        posShift = (int) ((frameNo - curFrame) * getFrameSize());
        curFrame = frameNo;
    }

    @Override
    public long getFrameCount() {
        return totalFrames;
    }

    @Override
    public DemuxerTrackMeta getMeta() {
        if (getSampleEntries()[0] instanceof AudioSampleEntry) {
            AudioSampleEntry ase = (AudioSampleEntry) getSampleEntries()[0];
            AudioCodecMeta audioCodecMeta = org.jcodec.common.AudioCodecMeta.fromAudioFormat(ase.getFormat());
            return new DemuxerTrackMeta(TrackType.AUDIO, Codec.codecByFourcc(getFourcc()), (double) duration / timescale, null, totalFrames,
                null, null, audioCodecMeta);
        } else {
            System.out.println("stan");
            return null;
        }
    }
}
