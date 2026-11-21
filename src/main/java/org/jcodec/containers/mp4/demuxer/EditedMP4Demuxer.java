package org.jcodec.containers.mp4.demuxer;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.containers.mp4.MP4Packet;
import org.jcodec.containers.mp4.MP4Util;
import org.jcodec.containers.mp4.MP4Util.Movie;
import org.jcodec.containers.mp4.boxes.AudioSampleEntry;
import org.jcodec.containers.mp4.boxes.Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsets64Box;
import org.jcodec.containers.mp4.boxes.ChunkOffsetsBox;
import org.jcodec.containers.mp4.boxes.CompositionOffsetsBox;
import org.jcodec.containers.mp4.boxes.Edit;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.NodeBox;
import org.jcodec.containers.mp4.boxes.SampleEntry;
import org.jcodec.containers.mp4.boxes.SampleSizesBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox.SampleToChunkEntry;
import org.jcodec.containers.mp4.boxes.SyncSamplesBox;
import org.jcodec.containers.mp4.boxes.TimeToSampleBox;
import org.jcodec.containers.mp4.boxes.TimeToSampleBox.TimeToSampleEntry;
import org.jcodec.containers.mp4.boxes.TrakBox;

/**
 * This class is part of JCodec ( www.jcodec.org ) This software is distributed
 * under FreeBSD License
 *
 * Edit-aware MP4 demuxing session.
 *
 * Opens a movie containing exactly one ordinary video track and one signed
 * 16-bit PCM audio track and presents both tracks on a single common session
 * clock (the LCM of the movie, video and audio timescales). Edit lists made of
 * rate-1 media segments and empty (media time -1) segments are honoured
 * directly: media segments may repeat or play backwards. The payload bytes
 * still come from the native {@link MP4DemuxerTrack} /
 * {@link PCMMP4DemuxerTrack} readers through their real chunk/sample
 * positioning logic; this class only owns the edit state and the session
 * cursors.
 *
 * The input channel stays owned by the caller; the session never closes it and
 * never reads media while positioning.
 *
 * @author The JCodec project
 */
public class EditedMP4Demuxer {

    /**
     * One normalized edit list entry. Time fields are stored both in the track
     * media timescale and on the common session clock.
     */
    static final class Segment {
        final boolean empty;
        final long startC;
        final long endC;
        final long mediaStart;
        final long mediaEnd;

        private Segment(boolean empty, long startC, long endC, long mediaStart, long mediaEnd) {
            this.empty = empty;
            this.startC = startC;
            this.endC = endC;
            this.mediaStart = mediaStart;
            this.mediaEnd = mediaEnd;
        }

        static Segment empty(long startC, long endC) {
            return new Segment(true, startC, endC, -1, -1);
        }

        static Segment media(long startC, long endC, long mediaStart, long mediaEnd) {
            return new Segment(false, startC, endC, mediaStart, mediaEnd);
        }
    }

    /**
     * Common session cursor behaviour: tracks expose the next output position on
     * the common clock, hand out {@link MP4Packet}s and support frame-granular
     * repositioning without reading media.
     */
    public abstract static class EditedTrack {
        final long commonClock;
        final int trackTimescale;
        final long clockStep;
        final Segment[] segments;
        private final long[] segmentStarts;
        final long durationC;

        int segIdx;
        long segPosC;

        EditedTrack(int trackTimescale, long commonTimescale, List<Segment> segmentList, long durationC) {
            this.commonClock = commonTimescale;
            this.trackTimescale = trackTimescale;
            this.clockStep = commonTimescale / trackTimescale;
            this.segments = segmentList.toArray(new Segment[0]);
            this.segmentStarts = new long[this.segments.length];
            for (int i = 0; i < this.segments.length; i++)
                this.segmentStarts[i] = this.segments[i].startC;
            this.durationC = durationC;
        }

        /**
         * Position of the next output packet on the common session clock; equal
         * to the movie duration once the track has been fully read.
         */
        public final long getPosition() {
            return segIdx >= segments.length ? durationC : segPosC;
        }

        public final MP4Packet nextFrame() throws IOException {
            return getNextFrame(null);
        }

        public abstract MP4Packet getNextFrame(ByteBuffer storage) throws IOException;

        /**
         * Computes the cursor for a common-clock position. The audio track
         * rejects positions that do not land on a whole sample frame. Pure
         * indexing work: no channel IO.
         */
        abstract void planSeek(long posC);

        final void applySeek(long posC) {
            int idx = Arrays.binarySearch(segmentStarts, posC);
            if (idx < 0)
                idx = -idx - 2;
            if (idx < 0)
                idx = 0;
            segIdx = idx;
            segPosC = posC;
        }

        final boolean atEnd() {
            return segIdx >= segments.length;
        }

        final MP4Packet packet(ByteBuffer data, long ptsC, long durationC, long frameNo, long mediaPts, int entryNo,
                long fileOff, int size, FrameType frameType) {
            return new MP4Packet(data, ptsC, (int) commonClock, durationC, frameNo, frameType, null, 0, mediaPts,
                    entryNo, fileOff, size, false);
        }

        /**
         * Narrows the flipped read view to exactly the bytes written by the
         * last call, leaving any pre-existing storage prefix out of the packet
         * data.
         */
        static ByteBuffer writtenView(ByteBuffer data, int origin, int size) {
            if (origin == 0 && data.remaining() == size)
                return data;
            ByteBuffer view = data.duplicate();
            view.position(origin);
            return view.slice();
        }
    }

    /**
     * Video session: whole source frames are always read intact, while the
     * presented interval is the frame display interval intersected with the
     * active edit; empty edits yield zero-byte gap packets.
     */
    public static final class EditedVideoTrack extends EditedTrack {
        private final MP4DemuxerTrack nativeTrack;
        private final long[] samplePts;

        EditedVideoTrack(MP4DemuxerTrack nativeTrack, int trackTimescale, long commonTimescale,
                List<Segment> segmentList, long durationC) {
            super(trackTimescale, commonTimescale, segmentList, durationC);
            this.nativeTrack = nativeTrack;
            this.samplePts = nativeTrack.samplePtsArray();
        }

        @Override
        public MP4Packet getNextFrame(ByteBuffer storage) throws IOException {
            if (atEnd())
                return null;
            Segment seg = segments[segIdx];
            long outPts = segPosC;

            if (seg.empty) {
                long outDur = seg.endC - segPosC;
                advance(seg.endC);
                return packet(ByteBuffer.allocate(0), outPts, outDur, -1, -1, 0, -1, 0, FrameType.KEY);
            }

            long mediaT = seg.mediaStart + (segPosC - seg.startC) / clockStep;
            int frame = Arrays.binarySearch(samplePts, mediaT);
            if (frame < 0)
                frame = -frame - 2;
            if (frame < 0 || frame >= samplePts.length) {
                // No source frame covers the rest of this segment: present the
                // remainder as a zero-byte gap, exactly like an empty edit.
                long outDur = seg.endC - segPosC;
                advance(seg.endC);
                return packet(ByteBuffer.allocate(0), outPts, outDur, -1, -1, 0, -1, 0, FrameType.KEY);
            }
            long framePts = samplePts[frame];
            long frameDur = nativeTrack.frameDurationAt(frame);
            long frameEnd = framePts + frameDur;
            long frameEndC = seg.startC + (frameEnd - seg.mediaStart) * clockStep;
            if (frameEndC <= segPosC) {
                // This frame ends before the requested output point.
                long outDur = seg.endC - segPosC;
                advance(seg.endC);
                return packet(ByteBuffer.allocate(0), outPts, outDur, -1, -1, 0, -1, 0, FrameType.KEY);
            }
            long outEndC = Math.min(frameEndC, seg.endC);
            long outDur = outEndC - segPosC;
            if (outDur <= 0)
                throw new IOException("Empty video output interval");
            int size = nativeTrack.getSampleSizes()[frame];

            ByteBuffer own = storage;
            if (own == null)
                own = ByteBuffer.allocate(size);
            else if (own.remaining() < size)
                throw new IllegalArgumentException("Buffer size is not enough to fit a video frame");
            int origin = own.position();

            nativeTrack.commitCursor(nativeTrack.cursorAtFrame(frame));
            MP4Packet source = nativeTrack.readRawFrame(own);
            if (source == null || source.getSize() != size)
                throw new IOException("Failed to read video frame " + frame);
            ByteBuffer data = writtenView(source.data, origin, size);

            advance(outEndC);

            return packet(data, outPts, outDur, source.getFrameNo(), source.getMediaPts(), source.getEntryNo(),
                    source.getFileOff(), size, source.frameType);
        }

        private void advance(long nextPosC) {
            Segment seg = segments[segIdx];
            if (nextPosC >= seg.endC) {
                segIdx++;
                segPosC = seg.endC;
            } else {
                segPosC = nextPosC;
            }
        }

        @Override
        void planSeek(long posC) {
            // Video may land inside a frame; every common-clock position is
            // legal, the first emitted packet is simply clipped.
        }
    }

    /**
     * PCM session: packets are cut out of real chunks on whole sample frame
     * boundaries, never cross an edit or a physical chunk, and empty edits
     * produce at most 4096 frames of signed zero silence per packet.
     */
    public static final class EditedAudioTrack extends EditedTrack {
        private final PCMMP4DemuxerTrack nativeTrack;
        private final long[] chunkFirstFrame;
        private final int frameSize;
        private final int channels;

        EditedAudioTrack(PCMMP4DemuxerTrack nativeTrack, int trackTimescale, long commonTimescale,
                List<Segment> segmentList, long durationC, int channels) {
            super(trackTimescale, commonTimescale, segmentList, durationC);
            this.nativeTrack = nativeTrack;
            this.chunkFirstFrame = nativeTrack.chunkFirstFrameArray();
            this.frameSize = nativeTrack.getPcmFrameSize();
            this.channels = channels;
        }

        public int getChannels() {
            return channels;
        }

        public int getFrameSize() {
            return frameSize;
        }

        @Override
        public MP4Packet getNextFrame(ByteBuffer storage) throws IOException {
            if (atEnd())
                return null;
            Segment seg = segments[segIdx];
            long outPts = segPosC;

            if (seg.empty) {
                long remainFrames = (seg.endC - segPosC) / clockStep;
                int frames = (int) Math.min(4096, remainFrames);
                if (storage != null) {
                    int fitting = storage.remaining() / frameSize;
                    if (fitting <= 0)
                        throw new IllegalArgumentException("Buffer size is not enough to fit a PCM frame");
                    frames = Math.min(frames, fitting);
                }
                if (frames <= 0)
                    throw new IOException("Empty audio output interval");
                long outDur = frames * clockStep;
                advance(segPosC + outDur);
                ByteBuffer silence = ByteBuffer.allocate(frames * frameSize);
                return packet(silence, outPts, outDur, -1, -1, 0, -1, frames * frameSize, FrameType.KEY);
            }

            long mediaFrame = seg.mediaStart + (segPosC - seg.startC) / clockStep;
            long segRemain = seg.mediaEnd - mediaFrame;

            if (mediaFrame >= nativeTrack.getTotalMediaFrames() || segRemain <= 0) {
                // The declared segment runs past the available media: fill the
                // remainder with signed silence in bounded packets.
                long remainFrames = (seg.endC - segPosC) / clockStep;
                int silenceFrames = (int) Math.min(4096, remainFrames);
                if (storage != null) {
                    int fitting = storage.remaining() / frameSize;
                    if (fitting <= 0)
                        throw new IllegalArgumentException("Buffer size is not enough to fit a PCM frame");
                    silenceFrames = Math.min(silenceFrames, fitting);
                }
                if (silenceFrames <= 0) {
                    segIdx++;
                    segPosC = seg.endC;
                    return getNextFrame(storage);
                }
                long outDur = silenceFrames * clockStep;
                advance(segPosC + outDur);
                return packet(ByteBuffer.allocate(silenceFrames * frameSize), outPts, outDur, -1, -1, 0, -1,
                        silenceFrames * frameSize, FrameType.KEY);
            }

            int chunk = Arrays.binarySearch(chunkFirstFrame, mediaFrame);
            if (chunk < 0)
                chunk = -chunk - 2;
            if (chunk < 0)
                throw new IOException("Audio frame outside any physical chunk");
            long chunkEnd = chunk + 1 < chunkFirstFrame.length ? chunkFirstFrame[chunk + 1]
                    : nativeTrack.getTotalMediaFrames();
            long chunkRemain = chunkEnd - mediaFrame;

            int frames = (int) Math.min(segRemain, chunkRemain);
            if (frames <= 0)
                throw new IOException("Empty audio output interval");

            ByteBuffer own = storage;
            if (own == null) {
                own = ByteBuffer.allocate(frames * frameSize);
            } else {
                int fitting = own.remaining() / frameSize;
                if (fitting <= 0)
                    throw new IllegalArgumentException("Buffer size is not enough to fit a PCM frame");
                frames = Math.min(frames, fitting);
            }
            int origin = own.position();

            nativeTrack.commitCursor(nativeTrack.cursorAtFrame(mediaFrame));
            MP4Packet source = nativeTrack.readRawFrames(own, frames);
            if (source == null || source.getSize() != frames * frameSize)
                throw new IOException("Failed to read PCM frames at media frame " + mediaFrame);
            ByteBuffer data = writtenView(source.data, origin, frames * frameSize);

            long outDur = frames * clockStep;
            long nextPosC = segPosC + outDur;
            if (nextPosC >= seg.endC) {
                segIdx++;
                segPosC = seg.endC;
            } else {
                segPosC = nextPosC;
            }

            return packet(data, outPts, outDur, source.getFrameNo(), source.getMediaPts(), source.getEntryNo(),
                    source.getFileOff(), frames * frameSize, FrameType.KEY);
        }

        private void advance(long nextPosC) {
            Segment seg = segments[segIdx];
            if (nextPosC >= seg.endC) {
                segIdx++;
                segPosC = seg.endC;
            } else {
                segPosC = nextPosC;
            }
        }

        @Override
        void planSeek(long posC) {
            if (posC % clockStep != 0)
                throw new IllegalArgumentException("PCM seek must land on a whole sample frame boundary");
        }
    }

    private final MovieBox movie;
    private final long commonClock;
    private final long movieTimescale;
    private final long movieDurationC;
    private final EditedVideoTrack video;
    private final EditedAudioTrack audio;

    private EditedMP4Demuxer(MovieBox movie, long commonClock, long movieDurationC, EditedVideoTrack video,
            EditedAudioTrack audio) {
        this.movie = movie;
        this.commonClock = commonClock;
        this.movieTimescale = movie.getTimescale();
        this.movieDurationC = movieDurationC;
        this.video = video;
        this.audio = audio;
    }

    /**
     * Opens the edit-aware session.
     *
     * @throws IOException
     *             the movie is missing, uses features outside the supported
     *             envelope, contains broken sample tables or edits, or any time
     *             computation overflows.
     */
    public static EditedMP4Demuxer create(SeekableByteChannel input) throws IOException {
        if (input == null)
            throw new IOException("Input channel is null");
        Movie parsed = MP4Util.parseFullMovieChannel(input);
        if (parsed == null || parsed.getMoov() == null)
            throw new IOException("Could not find movie meta information box");
        MovieBox movie = parsed.getMoov();

        TrakBox[] traks = movie.getTracks();
        TrakBox videoTrak = null;
        TrakBox audioTrak = null;
        for (TrakBox trak : traks) {
            if (trak.isVideo()) {
                if (videoTrak != null)
                    throw new IOException("Only one video track is supported");
                videoTrak = trak;
            } else if (trak.isAudio()) {
                if (audioTrak != null)
                    throw new IOException("Only one audio track is supported");
                audioTrak = trak;
            } else {
                throw new IOException("Unsupported track type in edited playback session");
            }
        }
        if (videoTrak == null || audioTrak == null)
            throw new IOException("Edited playback requires one video and one PCM audio track");

        int movieTs = movie.getTimescale();
        int videoTs = videoTrak.getTimescale();
        int audioTs = audioTrak.getTimescale();
        if (movieTs <= 0 || videoTs <= 0 || audioTs <= 0)
            throw new IOException("Timescales must be positive");
        long clock;
        try {
            clock = lcm(lcm(movieTs, videoTs), audioTs);
        } catch (ArithmeticException e) {
            throw new IOException("Common timescale computation overflow", e);
        }
        if (clock > Integer.MAX_VALUE)
            throw new IOException("Common timescale does not fit into a positive int: " + clock);

        long movieDur = movie.getDuration();
        if (movieDur <= 0)
            throw new IOException("Movie duration must be positive");
        long movieDurC = scaleExact(movieDur, clock, movieTs, "movie duration");

        VideoSetup videoSetup = validateAndBuildVideo(movie, videoTrak, input, clock, movieDurC);
        AudioSetup audioSetup = validateAndBuildAudio(movie, audioTrak, input, clock, movieDurC);

        return new EditedMP4Demuxer(movie, clock, movieDurC, videoSetup.track, audioSetup.track);
    }

    public EditedVideoTrack getVideoTrack() {
        return video;
    }

    public EditedAudioTrack getAudioTrack() {
        return audio;
    }

    public long getTimescale() {
        return commonClock;
    }

    public long getDuration() {
        return movieDurationC;
    }

    /**
     * Repositions both tracks at the same movie time. Every validation runs for
     * both tracks before either cursor moves, so a rejected seek leaves both
     * read positions untouched. No media payload is read.
     *
     * @return false for negative or past-the-end positions and for PCM
     *         positions that do not align with a whole sample frame
     */
    public boolean seekMovie(long movieTick) {
        if (movieTick < 0 || movieTick >= movie.getDuration())
            return false;
        long scaled;
        try {
            scaled = Math.multiplyExact(movieTick, commonClock);
        } catch (ArithmeticException e) {
            return false;
        }
        if (scaled % movieTimescale != 0)
            return false;
        long posC = scaled / movieTimescale;

        try {
            video.planSeek(posC);
            audio.planSeek(posC);
        } catch (RuntimeException e) {
            return false;
        }
        video.applySeek(posC);
        audio.applySeek(posC);
        return true;
    }

    // ------------------------------------------------------------------
    // Validation and index construction
    // ------------------------------------------------------------------

    private static final class VideoSetup {
        final EditedVideoTrack track;

        VideoSetup(EditedVideoTrack track) {
            this.track = track;
        }
    }

    private static final class AudioSetup {
        final EditedAudioTrack track;

        AudioSetup(EditedAudioTrack track) {
            this.track = track;
        }
    }

    private static VideoSetup validateAndBuildVideo(MovieBox movie, TrakBox trak, SeekableByteChannel input,
            long clock, long movieDurC) throws IOException {
        SampleEntry[] entries = NodeBox.findAllPath(trak, SampleEntry.class,
                new String[] { "mdia", "minf", "stbl", "stsd", null });
        if (entries.length != 1)
            throw new IOException("Video track must contain a single self-contained sample description");
        if (NodeBox.findFirstPath(trak, CompositionOffsetsBox.class, Box.path("mdia.minf.stbl.ctts")) != null)
            throw new IOException("Composition offsets are not supported");

        TimeToSampleEntry[] stts = requireStts(trak);
        SampleToChunkEntry[] stsc = requireStsc(trak);
        long[] offsets = requireChunkOffsets(trak);
        SampleSizesBox stsz = NodeBox.findFirstPath(trak, SampleSizesBox.class, Box.path("mdia.minf.stbl.stsz"));
        if (stsz == null)
            throw new IOException("Video stsz box is missing");
        SyncSamplesBox stss = NodeBox.findFirstPath(trak, SyncSamplesBox.class, Box.path("mdia.minf.stbl.stss"));

        int sampleCount = stsz.getDefaultSize() != 0 ? stsz.getCount() : stsz.getSizes().length;
        if (sampleCount <= 0)
            throw new IOException("Video track has no samples");
        long sttsSamples = 0;
        long mediaDuration = 0;
        for (TimeToSampleEntry e : stts) {
            if (e.getSampleCount() <= 0 || e.getSampleDuration() <= 0)
                throw new IOException("Corrupt video stts entry");
            sttsSamples = addExact(sttsSamples, e.getSampleCount(), "video stts count");
            mediaDuration = addExact(mediaDuration,
                    multiplyExact(e.getSampleCount(), e.getSampleDuration(), "video stts duration"),
                    "video media duration");
        }
        if (sttsSamples != sampleCount)
            throw new IOException("Video stts sample count does not match stsz");
        if (mediaDuration != trak.getMediaDuration())
            throw new IOException("Video media duration does not match stts");

        validateChunkStructure(stsc, offsets, sampleCount, entries.length);
        if (stss != null && stss.getSyncSamples().length != sampleCount)
            throw new IOException("All video samples must be independently decodable");

        MP4DemuxerTrack nativeTrack = new MP4DemuxerTrack(movie, trak, input);
        nativeTrack.buildSeekIndex();

        List<Segment> segments = buildSegments(trak, trak.getTimescale(), clock, trak.getMediaDuration(), false,
                movieDurC);
        if (segments.isEmpty())
            throw new IOException("Video track has no playable timeline");
        EditedVideoTrack track = new EditedVideoTrack(nativeTrack, trak.getTimescale(), clock, segments, movieDurC);
        return new VideoSetup(track);
    }

    private static AudioSetup validateAndBuildAudio(MovieBox movie, TrakBox trak, SeekableByteChannel input,
            long clock, long movieDurC) throws IOException {
        SampleEntry[] entries = NodeBox.findAllPath(trak, SampleEntry.class,
                new String[] { "mdia", "minf", "stbl", "stsd", null });
        if (entries.length != 1 || !(entries[0] instanceof AudioSampleEntry))
            throw new IOException("Audio track must contain a single PCM sample description");
        AudioSampleEntry ase = (AudioSampleEntry) entries[0];
        String fourcc = ase.getHeader().getFourcc();
        if (!"sowt".equals(fourcc) && !"twos".equals(fourcc))
            throw new IOException("Only sowt/twos PCM is supported: " + fourcc);
        int channels = ase.getChannelCount();
        if (channels != 1 && channels != 2)
            throw new IOException("PCM must be mono or stereo");
        if (ase.getSampleSize() != 16)
            throw new IOException("PCM must be signed 16-bit");
        int rate = Math.round(ase.getSampleRate());
        if (rate != trak.getTimescale() || rate <= 0)
            throw new IOException("PCM track timescale must equal the sample rate");

        TimeToSampleEntry[] stts = requireStts(trak);
        SampleToChunkEntry[] stsc = requireStsc(trak);
        long[] offsets = requireChunkOffsets(trak);
        SampleSizesBox stsz = NodeBox.findFirstPath(trak, SampleSizesBox.class, Box.path("mdia.minf.stbl.stsz"));
        if (stsz == null)
            throw new IOException("Audio stsz box is missing");
        int frameSize = 2 * channels;
        if (stsz.getDefaultSize() != frameSize)
            throw new IOException("PCM samples must have a fixed frame size of " + frameSize + " bytes");

        long sttsFrames = 0;
        for (TimeToSampleEntry e : stts) {
            if (e.getSampleCount() <= 0 || e.getSampleDuration() != 1)
                throw new IOException("Each PCM stts sample must be exactly one sample frame");
            sttsFrames = addExact(sttsFrames, e.getSampleCount(), "pcm stts count");
        }
        if (sttsFrames != stsz.getCount())
            throw new IOException("PCM stts/stsz sample counts disagree");
        if (sttsFrames != trak.getMediaDuration())
            throw new IOException("PCM media duration must equal the sample frame count");

        validateChunkStructure(stsc, offsets, stsz.getCount(), entries.length);

        PCMMP4DemuxerTrack nativeTrack = new PCMMP4DemuxerTrack(movie, trak, input);
        nativeTrack.buildSeekIndex();
        if (nativeTrack.getTotalMediaFrames() != stsz.getCount())
            throw new IOException("PCM stsc chunk frame count does not match the sample count");

        List<Segment> segments = buildSegments(trak, trak.getTimescale(), clock, trak.getMediaDuration(), true,
                movieDurC);
        if (segments.isEmpty())
            throw new IOException("Audio track has no playable timeline");
        EditedAudioTrack track = new EditedAudioTrack(nativeTrack, trak.getTimescale(), clock, segments, movieDurC,
                channels);
        return new AudioSetup(track);
    }

    private static TimeToSampleEntry[] requireStts(TrakBox trak) throws IOException {
        TimeToSampleBox box = NodeBox.findFirstPath(trak, TimeToSampleBox.class, Box.path("mdia.minf.stbl.stts"));
        if (box == null || box.getEntries().length == 0)
            throw new IOException("stts box is missing or empty");
        return box.getEntries();
    }

    private static SampleToChunkEntry[] requireStsc(TrakBox trak) throws IOException {
        SampleToChunkBox box = NodeBox.findFirstPath(trak, SampleToChunkBox.class, Box.path("mdia.minf.stbl.stsc"));
        if (box == null || box.getSampleToChunk().length == 0)
            throw new IOException("stsc box is missing or empty");
        return box.getSampleToChunk();
    }

    private static long[] requireChunkOffsets(TrakBox trak) throws IOException {
        ChunkOffsetsBox stco = NodeBox.findFirstPath(trak, ChunkOffsetsBox.class, Box.path("mdia.minf.stbl.stco"));
        ChunkOffsets64Box co64 = NodeBox.findFirstPath(trak, ChunkOffsets64Box.class, Box.path("mdia.minf.stbl.co64"));
        long[] offsets = stco != null ? stco.getChunkOffsets() : co64 == null ? null : co64.getChunkOffsets();
        if (offsets == null || offsets.length == 0)
            throw new IOException("Chunk offset table is missing or empty");
        for (long offset : offsets)
            if (offset < 0)
                throw new IOException("Negative chunk offset");
        return offsets;
    }

    private static void validateChunkStructure(SampleToChunkEntry[] stsc, long[] offsets, long sampleCount,
            int entryCount) throws IOException {
        if (stsc[0].getFirst() != 1)
            throw new IOException("stsc must start at the first chunk");
        long total = 0;
        for (int i = 0; i < stsc.length; i++) {
            if (stsc[i].getCount() <= 0)
                throw new IOException("stsc sample count must be positive");
            if (stsc[i].getEntry() < 1 || stsc[i].getEntry() > entryCount)
                throw new IOException("stsc references an unknown sample entry");
            if (i > 0 && stsc[i].getFirst() <= stsc[i - 1].getFirst())
                throw new IOException("stsc first-chunk values must increase");
            long chunksRun = i + 1 < stsc.length ? stsc[i + 1].getFirst() - stsc[i].getFirst()
                    : offsets.length - stsc[i].getFirst() + 1;
            if (chunksRun <= 0)
                throw new IOException("stsc run does not cover the chunk table");
            total = addExact(total, multiplyExact(chunksRun, stsc[i].getCount(), "stsc run samples"),
                    "chunk sample total");
        }
        if (total != sampleCount)
            throw new IOException("Samples covered by stsc do not match stsz");
    }

    /**
     * Normalizes the edit list into common-clock segments. A missing edit list
     * means one full-length rate-1 media segment. Every rate, duration and
     * boundary is validated; PCM additionally requires whole sample-frame
     * alignment, which is automatic when one stts sample is one frame.
     */
    private static List<Segment> buildSegments(TrakBox trak, int trackTs, long clock, long mediaDuration,
            boolean pcm, long movieDurC) throws IOException {
        List<Edit> edits = trak.getEdits();
        List<Segment> result = new ArrayList<Segment>();
        long step = clock / trackTs;
        if (step <= 0)
            throw new IOException("Common clock is not a multiple of track timescale");

        if (edits == null || edits.isEmpty()) {
            long endC = scaleExact(mediaDuration, clock, trackTs, "implicit edit duration");
            result.add(Segment.media(0, endC, 0, mediaDuration));
            if (endC != movieDurC)
                throw new IOException("Edit-less track duration does not match the movie duration");
            return result;
        }

        long cursorC = 0;
        long playedDuration = 0;
        for (Edit edit : edits) {
            if (edit.getRate() != 1.0f)
                throw new IOException("Only rate=1 edits are supported");
            long duration = edit.getDuration();
            if (duration <= 0)
                throw new IOException("Edit durations must be positive");
            long durC = scaleExact(duration, clock, trackTs, "edit duration");
            long endC = addExact(cursorC, durC, "edit list duration");
            if (endC > movieDurC) {
                // The edit list runs past the movie timeline: clip the last
                // active segment at the mvhd boundary and ignore what follows.
                durC = movieDurC - cursorC;
                if (durC <= 0)
                    break;
                endC = movieDurC;
                duration = durC / step;
                if (durC % step != 0)
                    throw new IOException("Edit clipping does not land on a track time boundary");
            }
            long mediaTime = edit.getMediaTime();
            if (mediaTime == -1) {
                if (pcm && durC % step != 0)
                    throw new IOException("PCM empty edit does not end on a sample frame boundary");
                result.add(Segment.empty(cursorC, endC));
            } else {
                if (mediaTime < 0 || mediaTime >= mediaDuration)
                    throw new IOException("Edit starts outside the media timeline");
                long segEnd = addExact(mediaTime, duration, "edit media end");
                // One stts sample is one PCM frame, so any integral media time
                // is a frame boundary; only the edit duration needs checking.
                if (pcm && durC % step != 0)
                    throw new IOException("PCM edit does not land on a sample frame boundary");
                if (segEnd > mediaDuration)
                    // Keep the declared edit span but never present past the end
                    // of the available media.
                    segEnd = mediaDuration;
                result.add(Segment.media(cursorC, endC, mediaTime, segEnd));
            }
            cursorC = endC;
            playedDuration = addExact(playedDuration, duration, "track edited duration");
            if (cursorC == movieDurC)
                break;
        }
        if (playedDuration != trak.getDuration() && cursorC != movieDurC)
            throw new IOException("Edit list duration does not match track duration");
        if (cursorC != movieDurC)
            throw new IOException("Track edited duration does not cover the movie duration");
        return result;
    }

    private static long scaleExact(long value, long clock, long timescale, String what) throws IOException {
        try {
            long scaled = multiplyExact(value, clock, what);
            if (scaled % timescale != 0)
                throw new IOException(what + " cannot be represented exactly on the common clock");
            return scaled / timescale;
        } catch (ArithmeticException e) {
            throw new IOException(what + " computation overflow", e);
        }
    }

    private static long addExact(long a, long b, String what) throws IOException {
        try {
            return Math.addExact(a, b);
        } catch (ArithmeticException e) {
            throw new IOException(what + " overflow", e);
        }
    }

    private static long multiplyExact(long a, long b, String what) throws IOException {
        try {
            return Math.multiplyExact(a, b);
        } catch (ArithmeticException e) {
            throw new IOException(what + " overflow", e);
        }
    }

    private static long gcd(long a, long b) {
        while (b != 0) {
            long t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    private static long lcm(long a, long b) {
        return Math.multiplyExact(a / gcd(a, b), b);
    }
}
