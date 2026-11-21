package org.jcodec.containers.mp4.demuxer;

import static task_support.Check.check;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.jcodec.common.AudioFormat;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.common.model.Rational;
import org.jcodec.common.model.Size;
import org.jcodec.common.model.Unit;
import org.jcodec.containers.mp4.MP4Packet;
import org.jcodec.containers.mp4.MP4TrackType;
import org.jcodec.containers.mp4.boxes.AudioSampleEntry;
import org.jcodec.containers.mp4.boxes.ChunkOffsets64Box;
import org.jcodec.containers.mp4.boxes.Edit;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.SampleDescriptionBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox;
import org.jcodec.containers.mp4.boxes.SampleToChunkBox.SampleToChunkEntry;
import org.jcodec.containers.mp4.boxes.TrakBox;
import org.jcodec.containers.mp4.boxes.VideoSampleEntry;
import org.jcodec.containers.mp4.muxer.MP4Muxer;
import org.jcodec.containers.mp4.muxer.MP4MuxerTrack;
import org.jcodec.containers.mp4.muxer.PCMMP4MuxerTrack;
import org.jcodec.common.model.Packet;

/**
 * Hand-checked verification of the edit-aware MP4 demuxing session. The movie
 * fixtures are assembled from scratch with explicit chunk grouping, in-chunk
 * cuts, reversed/repeated media edits and staggered empty edits so every
 * emitted packet can be compared against a manually derived timeline.
 */
public class EditedPlaybackTest {

    // ------------------------------------------------------------------
    // Tiny controllable channel: fails on demand and can emulate a short file
    // ------------------------------------------------------------------

    static final class FlakyChannel implements SeekableByteChannel {
        private final SeekableByteChannel delegate;
        private int failOnRead = -1;
        private int reads;
        private int truncatedBy;

        FlakyChannel(SeekableByteChannel delegate) {
            this.delegate = delegate;
        }

        void failNextRead() {
            failOnRead = reads;
        }

        void clearFailure() {
            failOnRead = -1;
        }

        void truncate(int bytes) {
            this.truncatedBy = bytes;
        }

        @Override
        public int read(ByteBuffer dst) throws IOException {
            if (failOnRead == reads)
                throw new IOException("Injected read failure " + reads);
            if (truncatedBy > 0) {
                long remaining = delegate.size() - truncatedBy - delegate.position();
                if (remaining <= 0)
                    return -1;
                int oldLimit = dst.limit();
                int capped = (int) Math.min(oldLimit, dst.position() + remaining);
                dst.limit(capped);
                try {
                    reads++;
                    return delegate.read(dst);
                } finally {
                    dst.limit(oldLimit);
                }
            }
            reads++;
            return delegate.read(dst);
        }

        @Override
        public int write(ByteBuffer src) throws IOException {
            return delegate.write(src);
        }

        @Override
        public long position() throws IOException {
            return delegate.position();
        }

        @Override
        public SeekableByteChannel setPosition(long newPosition) throws IOException {
            return delegate.setPosition(newPosition);
        }

        @Override
        public long size() throws IOException {
            return delegate.size() - truncatedBy;
        }

        @Override
        public SeekableByteChannel truncate(long size) throws IOException {
            return delegate.truncate(size);
        }

        @Override
        public boolean isOpen() {
            return delegate.isOpen();
        }

        @Override
        public void close() throws IOException {
            delegate.close();
        }
    }

    // ------------------------------------------------------------------
    // Fixture assembly
    // ------------------------------------------------------------------

    static File newFile(String name) throws IOException {
        File dir = new File(".task-build");
        dir.mkdirs();
        File f = new File(dir, name);
        java.nio.file.Files.deleteIfExists(f.toPath());
        return f;
    }

    static ByteBuffer videoSample(int i, int size) {
        byte[] b = new byte[size];
        for (int j = 0; j < size; j++)
            b[j] = (byte) (0x40 + i + j);
        return ByteBuffer.wrap(b);
    }

    static int videoSampleSize(int i) {
        return 3 + (i % 2);
    }

    static void putPcmFrame(ByteBuffer b, int i, boolean bigEndian) {
        int value = 0x0100 + i;
        b.put((byte) (value & 0xff));
        b.put((byte) ((value >> 8) & 0xff));
        if (bigEndian) {
            byte lo = b.get(b.position() - 2);
            byte hi = b.get(b.position() - 1);
            b.put(b.position() - 2, hi);
            b.put(b.position() - 1, lo);
        }
    }

    static void regroup(TrakBox trak, long[] baseOffsets, int[] counts) {
        SampleToChunkEntry[] stsc = new SampleToChunkEntry[counts.length];
        for (int i = 0; i < counts.length; i++)
            stsc[i] = new SampleToChunkEntry(i + 1, counts[i], 1);
        trak.getStbl().replace("stsc", SampleToChunkBox.createSampleToChunkBox(stsc));
        trak.getStbl().replace("co64", ChunkOffsets64Box.createChunkOffsets64Box(baseOffsets));
    }

    static long[] offsetsAt(long[] original, int[] indices) {
        long[] out = new long[indices.length];
        for (int i = 0; i < indices.length; i++)
            out[i] = original[indices[i]];
        return out;
    }

    /**
     * Main rig: movie timescale 12, video timescale 6, PCM timescale 8, common
     * clock LCM = 24. Movie duration 24 movie ticks = 48 common ticks.
     *
     * Video: 12 independent frames (alternating 3/4 bytes), physical chunks of
     * 4 frames, edits empty / media[6..9) / media[0..3) / media[6..9).
     * PCM: 16 mono sowt frames, physical chunks [0..6) [6..12) [12..16),
     * edits media[3..7) / empty / media[0..4) / media[3..7).
     */
    static File buildRigA() throws IOException {
        File f = newFile("rig-a.mp4");
        try (SeekableByteChannel out = NIOUtils.writableChannel(f)) {
            MP4Muxer mux = MP4Muxer.createMP4MuxerToChannel(out);
            MP4MuxerTrack v = mux.addTrack(new MP4MuxerTrack(1, MP4TrackType.VIDEO));
            v.addSampleEntry(VideoSampleEntry.videoSampleEntry("mp4v", new Size(2, 2), "rig"));
            for (int i = 0; i < 12; i++)
                v.addFrame(Packet.createPacket(videoSample(i, videoSampleSize(i)), i, 6, 1, i, FrameType.KEY, null));

            PCMMP4MuxerTrack a = mux.addPCMAudioTrack(new AudioFormat(8, 16, 1, true, false));
            a.setTgtChunkDuration(new Rational(1, 4), Unit.SEC);
            // Three writes become three physical chunks of 6, 6 and 4 frames.
            for (int[] run : new int[][] { { 0, 6 }, { 6, 12 }, { 12, 16 } }) {
                ByteBuffer pcm = ByteBuffer.allocate((run[1] - run[0]) * 2);
                for (int i = run[0]; i < run[1]; i++)
                    putPcmFrame(pcm, i, false);
                pcm.flip();
                a.addSamples(pcm);
            }

            MovieBox movie = mux.finalizeHeader();
            movie.getMovieHeader().setTimescale(12);
            movie.setDuration(24);
            TrakBox[] traks = movie.getTracks();

            long[] vOrig = traks[0].getCo64().getChunkOffsets();
            regroup(traks[0], offsetsAt(vOrig, new int[] { 0, 4, 8 }), new int[] { 4, 4, 4 });

            traks[0].setEdits(Arrays.asList(new Edit(3, -1, 1), new Edit(3, 6, 1), new Edit(3, 0, 1),
                    new Edit(3, 6, 1)));
            traks[1].setEdits(Arrays.asList(new Edit(4, 3, 1), new Edit(4, -1, 1), new Edit(4, 0, 1),
                    new Edit(4, 3, 1)));
            traks[0].setDuration(12);
            traks[1].setDuration(16);
            mux.storeHeader(movie);
        }
        return f;
    }

    /** Stereo little-endian PCM, timescales 10 / 5 / 20, common clock 20. */
    static File buildRigB() throws IOException {
        File f = newFile("rig-b.mp4");
        try (SeekableByteChannel out = NIOUtils.writableChannel(f)) {
            MP4Muxer mux = MP4Muxer.createMP4MuxerToChannel(out);
            MP4MuxerTrack v = mux.addTrack(new MP4MuxerTrack(1, MP4TrackType.VIDEO));
            v.addSampleEntry(VideoSampleEntry.videoSampleEntry("mp4v", new Size(2, 2), "rig-b"));
            for (int i = 0; i < 5; i++)
                v.addFrame(Packet.createPacket(videoSample(i, 3), i, 5, 1, i, FrameType.KEY, null));

            PCMMP4MuxerTrack a = mux.addPCMAudioTrack(new AudioFormat(20, 16, 2, true, false));
            a.setTgtChunkDuration(new Rational(1, 5), Unit.SEC);
            // Physical chunks of 8, 8 and 4 stereo frames.
            for (int[] run : new int[][] { { 0, 8 }, { 8, 16 }, { 16, 20 } }) {
                ByteBuffer pcm = ByteBuffer.allocate((run[1] - run[0]) * 4);
                for (int i = run[0]; i < run[1]; i++)
                    pcm.put((byte) i).put((byte) 1).put((byte) (i + 64)).put((byte) 2);
                pcm.flip();
                a.addSamples(pcm);
            }

            MovieBox movie = mux.finalizeHeader();
            movie.getMovieHeader().setTimescale(10);
            movie.setDuration(10);
            TrakBox[] traks = movie.getTracks();

            traks[0].setEdits(Arrays.asList(new Edit(2, 0, 1), new Edit(1, -1, 1), new Edit(2, 2, 1)));
            traks[1].setEdits(Arrays.asList(new Edit(4, -1, 1), new Edit(8, 0, 1), new Edit(8, 8, 1)));
            traks[0].setDuration(5);
            traks[1].setDuration(20);
            mux.storeHeader(movie);
        }
        return f;
    }

    /** Mono big-endian twos PCM, all three timescales equal to 10. */
    static File buildRigC() throws IOException {
        File f = newFile("rig-c.mp4");
        try (SeekableByteChannel out = NIOUtils.writableChannel(f)) {
            MP4Muxer mux = MP4Muxer.createMP4MuxerToChannel(out);
            MP4MuxerTrack v = mux.addTrack(new MP4MuxerTrack(1, MP4TrackType.VIDEO));
            v.addSampleEntry(VideoSampleEntry.videoSampleEntry("mp4v", new Size(2, 2), "rig-c"));
            for (int i = 0; i < 10; i++)
                v.addFrame(Packet.createPacket(videoSample(i, 3), i, 10, 1, i, FrameType.KEY, null));

            // The muxer only emits sowt; write sowt then swap the description
            // for an equivalent big-endian twos entry.
            PCMMP4MuxerTrack a = mux.addPCMAudioTrack(new AudioFormat(10, 16, 1, true, false));
            a.setTgtChunkDuration(new Rational(1, 2), Unit.SEC);
            // Two physical chunks of 5 frames each.
            for (int[] run : new int[][] { { 0, 5 }, { 5, 10 } }) {
                ByteBuffer pcm = ByteBuffer.allocate((run[1] - run[0]) * 2);
                for (int i = run[0]; i < run[1]; i++)
                    putPcmFrame(pcm, i, true);
                pcm.flip();
                a.addSamples(pcm);
            }

            MovieBox movie = mux.finalizeHeader();
            TrakBox[] traks = movie.getTracks();
            // Replace the sowt description with an equivalent twos one.
            SampleDescriptionBox stsd = traks[1].getStsd();
            stsd.getBoxes().clear();
            stsd.add(AudioSampleEntry.audioSampleEntry("twos", 1, 2, 1, 10, ByteOrder.BIG_ENDIAN));

            traks[0].setEdits(Arrays.asList(new Edit(4, 0, 1), new Edit(2, -1, 1), new Edit(4, 4, 1)));
            traks[1].setEdits(Arrays.asList(new Edit(2, -1, 1), new Edit(4, 0, 1), new Edit(4, 4, 1)));
            traks[0].setDuration(10);
            traks[1].setDuration(10);
            mux.storeHeader(movie);
        }
        return f;
    }

    /** No edit lists: implicit full-media playback on both tracks. */
    static File buildRigNoEdit() throws IOException {
        File f = newFile("rig-noedit.mp4");
        try (SeekableByteChannel out = NIOUtils.writableChannel(f)) {
            MP4Muxer mux = MP4Muxer.createMP4MuxerToChannel(out);
            MP4MuxerTrack v = mux.addTrack(new MP4MuxerTrack(1, MP4TrackType.VIDEO));
            v.addSampleEntry(VideoSampleEntry.videoSampleEntry("mp4v", new Size(2, 2), "no-edit"));
            for (int i = 0; i < 6; i++)
                v.addFrame(Packet.createPacket(videoSample(i, videoSampleSize(i)), i, 6, 1, i, FrameType.KEY, null));

            PCMMP4MuxerTrack a = mux.addPCMAudioTrack(new AudioFormat(12, 16, 1, true, false));
            a.setTgtChunkDuration(new Rational(1, 3), Unit.SEC);
            ByteBuffer pcm = ByteBuffer.allocate(12 * 2);
            for (int i = 0; i < 12; i++)
                putPcmFrame(pcm, i, false);
            pcm.flip();
            a.addSamples(pcm);

            MovieBox movie = mux.finalizeHeader();
            movie.getMovieHeader().setTimescale(12);
            movie.setDuration(12);
            TrakBox[] traks = movie.getTracks();
            traks[0].setDuration(12);
            traks[1].setDuration(12);
            mux.storeHeader(movie);
        }
        return f;
    }

    /** Large leading empty edit: silence must be split into <=4096 frame packets. */
    static File buildRigSilence() throws IOException {
        File f = newFile("rig-silence.mp4");
        try (SeekableByteChannel out = NIOUtils.writableChannel(f)) {
            MP4Muxer mux = MP4Muxer.createMP4MuxerToChannel(out);
            MP4MuxerTrack v = mux.addTrack(new MP4MuxerTrack(1, MP4TrackType.VIDEO));
            v.addSampleEntry(VideoSampleEntry.videoSampleEntry("mp4v", new Size(2, 2), "silence-rig"));
            for (int i = 0; i < 10; i++)
                v.addFrame(Packet.createPacket(videoSample(i, 3), i, 10, 1, i, FrameType.KEY, null));

            PCMMP4MuxerTrack a = mux.addPCMAudioTrack(new AudioFormat(10000, 16, 1, true, false));
            a.setTgtChunkDuration(new Rational(1, 10), Unit.SEC);
            // 5000 media frames in two writes (two physical chunks).
            for (int[] run : new int[][] { { 0, 2500 }, { 2500, 5000 } }) {
                ByteBuffer pcm = ByteBuffer.allocate((run[1] - run[0]) * 2);
                for (int i = run[0]; i < run[1]; i++)
                    putPcmFrame(pcm, i, false);
                pcm.flip();
                a.addSamples(pcm);
            }

            MovieBox movie = mux.finalizeHeader();
            movie.getMovieHeader().setTimescale(10000);
            movie.setDuration(10000);
            TrakBox[] traks = movie.getTracks();
            // Video: gap of 5000 ticks then all ten frames.
            traks[0].setEdits(Arrays.asList(new Edit(5000, -1, 1), new Edit(5000, 0, 1)));
            // Audio: 5000 frames of silence then all 5000 media frames.
            traks[1].setEdits(Arrays.asList(new Edit(5000, -1, 1), new Edit(5000, 0, 1)));
            traks[0].setDuration(10000);
            traks[1].setDuration(10000);
            mux.storeHeader(movie);
        }
        return f;
    }

    // ------------------------------------------------------------------
    // Hand-derived expectations for rig A
    // ------------------------------------------------------------------

    static final class ExpectedPacket {
        final long pts;
        final long duration;
        final long frameNo;
        final long mediaPts;
        final long fileOff;
        final int size;
        final boolean gap;

        ExpectedPacket(long pts, long duration, long frameNo, long mediaPts, long fileOff, int size, boolean gap) {
            this.pts = pts;
            this.duration = duration;
            this.frameNo = frameNo;
            this.mediaPts = mediaPts;
            this.fileOff = fileOff;
            this.size = size;
            this.gap = gap;
        }
    }

    static long videoOffsetRigA(long[] bases, int frame) {
        int chunk = frame / 4;
        int inside = frame % 4;
        long off = bases[chunk];
        for (int i = chunk * 4; i < chunk * 4 + inside; i++)
            off += videoSampleSize(i);
        return off;
    }

    static List<ExpectedPacket> expectedVideoRigA(long[] bases) {
        List<ExpectedPacket> l = new ArrayList<ExpectedPacket>();
        l.add(new ExpectedPacket(0, 12, -1, -1, -1, 0, true));
        l.add(new ExpectedPacket(12, 4, 6, 6, videoOffsetRigA(bases, 6), videoSampleSize(6), false));
        l.add(new ExpectedPacket(16, 4, 7, 7, videoOffsetRigA(bases, 7), videoSampleSize(7), false));
        l.add(new ExpectedPacket(20, 4, 8, 8, videoOffsetRigA(bases, 8), videoSampleSize(8), false));
        l.add(new ExpectedPacket(24, 4, 0, 0, videoOffsetRigA(bases, 0), videoSampleSize(0), false));
        l.add(new ExpectedPacket(28, 4, 1, 1, videoOffsetRigA(bases, 1), videoSampleSize(1), false));
        l.add(new ExpectedPacket(32, 4, 2, 2, videoOffsetRigA(bases, 2), videoSampleSize(2), false));
        l.add(new ExpectedPacket(36, 4, 6, 6, videoOffsetRigA(bases, 6), videoSampleSize(6), false));
        l.add(new ExpectedPacket(40, 4, 7, 7, videoOffsetRigA(bases, 7), videoSampleSize(7), false));
        l.add(new ExpectedPacket(44, 4, 8, 8, videoOffsetRigA(bases, 8), videoSampleSize(8), false));
        return l;
    }

    static List<ExpectedPacket> expectedAudioRigA(long[] bases) {
        List<ExpectedPacket> l = new ArrayList<ExpectedPacket>();
        // Edit media[3,7) cut at the physical chunk boundary between frames 5
        // and 6.
        l.add(new ExpectedPacket(0, 9, 3, 3, bases[0] + 3 * 2, 6, false));
        l.add(new ExpectedPacket(9, 3, 6, 6, bases[1], 2, false));
        l.add(new ExpectedPacket(12, 12, -1, -1, -1, 8, true));
        l.add(new ExpectedPacket(24, 12, 0, 0, bases[0], 8, false));
        l.add(new ExpectedPacket(36, 9, 3, 3, bases[0] + 3 * 2, 6, false));
        l.add(new ExpectedPacket(45, 3, 6, 6, bases[1], 2, false));
        return l;
    }

    static void assertBytesEqual(ByteBuffer data, byte[] expected, String what) {
        ByteBuffer d = data.duplicate();
        check(d.remaining() == expected.length, what + " length");
        byte[] actual = new byte[d.remaining()];
        d.get(actual);
        check(Arrays.equals(actual, expected), what + " bytes");
    }

    static byte[] expectedVideoBytes(int frame, int size) {
        byte[] b = new byte[size];
        for (int j = 0; j < size; j++)
            b[j] = (byte) (0x40 + frame + j);
        return b;
    }

    static byte[] expectedPcmBytes(int firstFrame, int frames, boolean bigEndian) {
        ByteBuffer b = ByteBuffer.allocate(frames * 2);
        for (int i = 0; i < frames; i++)
            putPcmFrame(b, firstFrame + i, bigEndian);
        b.flip();
        byte[] out = new byte[b.remaining()];
        b.get(out);
        return out;
    }

    static long[] chunkBases(File f, int track) throws IOException {
        MovieBox movie = org.jcodec.containers.mp4.MP4Util.parseFullMovieChannel(NIOUtils.readableChannel(f)).getMoov();
        return movie.getTracks()[track].getCo64().getChunkOffsets();
    }

    static void assertPacket(MP4Packet p, ExpectedPacket e, long clock, String label) {
        check(p != null, label + " not null");
        check(p.getTimescale() == clock, label + " timescale");
        check(p.getPts() == e.pts, label + " pts");
        check(p.getDuration() == e.duration, label + " duration");
        check(p.getFrameNo() == e.frameNo, label + " frameNo");
        check(p.getMediaPts() == e.mediaPts, label + " mediaPts");
        check(p.getFileOff() == e.fileOff, label + " fileOff");
        check(p.getSize() == e.size, label + " size");
        if (e.gap && e.size == 0) {
            check(p.getData().remaining() == 0, label + " zero-byte gap");
        }
    }

    // ------------------------------------------------------------------
    // Group 1: full timeline, bytes, addresses, alternate clocks
    // ------------------------------------------------------------------

    public static void timeline() throws Exception {
        File rig = buildRigA();
        long[] vBases = chunkBases(rig, 0);
        long[] aBases = chunkBases(rig, 1);
        List<ExpectedPacket> vExp = expectedVideoRigA(vBases);
        List<ExpectedPacket> aExp = expectedAudioRigA(aBases);

        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);
            check(d.getTimescale() == 24, "rig A common clock is LCM(12,6,8)=24");
            check(d.getDuration() == 48, "rig A duration on common clock");
            check(ch.isOpen(), "caller keeps channel ownership");

            List<MP4Packet> video = new ArrayList<MP4Packet>();
            MP4Packet p;
            while ((p = d.getVideoTrack().nextFrame()) != null)
                video.add(p);
            check(video.size() == vExp.size(), "video packet count");
            check(d.getVideoTrack().getPosition() == 48, "video position stops at duration");

            for (int i = 0; i < video.size(); i++) {
                MP4Packet got = video.get(i);
                ExpectedPacket e = vExp.get(i);
                assertPacket(got, e, 24, "video[" + i + "]");
                if (!e.gap)
                    assertBytesEqual(got.getData(), expectedVideoBytes((int) e.frameNo, e.size), "video[" + i + "]");
            }
            check(d.getVideoTrack().nextFrame() == null, "video stays at EOF");

            List<MP4Packet> audio = new ArrayList<MP4Packet>();
            while ((p = d.getAudioTrack().nextFrame()) != null)
                audio.add(p);
            check(audio.size() == aExp.size(), "audio packet count");
            check(d.getAudioTrack().getPosition() == 48, "audio position stops at duration");
            for (int i = 0; i < audio.size(); i++) {
                MP4Packet got = audio.get(i);
                ExpectedPacket e = aExp.get(i);
                assertPacket(got, e, 24, "audio[" + i + "]");
                if (e.gap) {
                    byte[] zeros = new byte[e.size];
                    assertBytesEqual(got.getData(), zeros, "silence[" + i + "]");
                } else {
                    int frames = e.size / 2;
                    assertBytesEqual(got.getData(), expectedPcmBytes((int) e.frameNo, frames, false), "audio[" + i + "]");
                }
            }

            // Interleaved reading over one shared channel must give the same
            // packets in the same order.
            check(d.seekMovie(0), "rewind both tracks");
            check(d.getVideoTrack().getPosition() == 0 && d.getAudioTrack().getPosition() == 0,
                    "rewind resets positions");
            int vi = 0, ai = 0;
            while (vi < vExp.size() || ai < aExp.size()) {
                if (vi < vExp.size()) {
                    MP4Packet vp = d.getVideoTrack().nextFrame();
                    assertPacket(vp, vExp.get(vi), 24, "interleaved video[" + vi + "]");
                    vi++;
                }
                if (ai < aExp.size()) {
                    MP4Packet ap = d.getAudioTrack().nextFrame();
                    assertPacket(ap, aExp.get(ai), 24, "interleaved audio[" + ai + "]");
                    ai++;
                }
            }
        }

        timelineRigB();
        timelineRigC();
        timelineNoEdit();
    }

    static void timelineRigB() throws Exception {
        File rig = buildRigB();
        long[] aBases = chunkBases(rig, 1);
        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);
            check(d.getTimescale() == 20, "rig B common clock LCM(10,5,20)=20");
            check(d.getDuration() == 20, "rig B duration");

            // Video: media[0,2), gap, media[2,5).
            MP4Packet p0 = d.getVideoTrack().nextFrame();
            check(p0.getPts() == 0 && p0.getDuration() == 4 && p0.getFrameNo() == 0, "rigB v0");
            MP4Packet p1 = d.getVideoTrack().nextFrame();
            check(p1.getPts() == 4 && p1.getDuration() == 4 && p1.getFrameNo() == 1, "rigB v1");
            MP4Packet gap = d.getVideoTrack().nextFrame();
            check(gap.getPts() == 8 && gap.getDuration() == 4 && gap.getFrameNo() == -1
                    && gap.getData().remaining() == 0, "rigB video gap");
            MP4Packet p2 = d.getVideoTrack().nextFrame();
            check(p2.getPts() == 12 && p2.getFrameNo() == 2, "rigB v2");
            check(d.getVideoTrack().nextFrame().getFrameNo() == 3, "rigB v3");
            // The final edit ends at media time 4, so source frame 4 is never
            // presented.
            check(d.getVideoTrack().nextFrame() == null, "rigB video EOF");

            // Audio: 4 stereo silence frames then two media segments.
            MP4Packet silence = d.getAudioTrack().nextFrame();
            check(silence.getPts() == 0 && silence.getDuration() == 4 && silence.getFrameNo() == -1
                    && silence.getSize() == 16, "rigB silence packet");
            byte[] zeros = new byte[16];
            assertBytesEqual(silence.getData(), zeros, "rigB silence bytes");
            MP4Packet a0 = d.getAudioTrack().nextFrame();
            check(a0.getPts() == 4 && a0.getFrameNo() == 0 && a0.getFileOff() == aBases[0]
                    && a0.getSize() == 8 * 4, "rigB first media chunk cut");
            MP4Packet a1 = d.getAudioTrack().nextFrame();
            check(a1.getFrameNo() == 8 && a1.getFileOff() == aBases[1], "rigB second segment starts at frame 8");
            check(d.getAudioTrack().getPosition() == 20, "rigB audio end position");
        }
    }

    static void timelineRigC() throws Exception {
        File rig = buildRigC();
        long[] aBases = chunkBases(rig, 1);
        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);
            check(d.getTimescale() == 10, "rig C common clock 10");
            check(d.getDuration() == 10, "rig C duration");

            MP4Packet silence = d.getAudioTrack().nextFrame();
            check(silence.getFrameNo() == -1 && silence.getDuration() == 2 && silence.getSize() == 4,
                    "rigC leading silence");
            MP4Packet a0 = d.getAudioTrack().nextFrame();
            check(a0.getFrameNo() == 0 && a0.getFileOff() == aBases[0] && a0.getSize() == 8,
                    "rigC big-endian chunk");
            // Big-endian payload of frame 0 (value 0x0100): 01 00.
            assertBytesEqual(a0.getData(), new byte[] { 1, 0, 1, 1, 1, 2, 1, 3 }, "rigC twos byte order");
            // The next media edit starts at frame 4, which is the last frame of
            // the first physical chunk, so it must arrive as its own cut packet.
            MP4Packet aCut = d.getAudioTrack().nextFrame();
            check(aCut.getFrameNo() == 4 && aCut.getFileOff() == aBases[0] + 4 * 2 && aCut.getSize() == 2,
                    "rigC chunk-internal cut");
            assertBytesEqual(aCut.getData(), expectedPcmBytes(4, 1, true), "rigC cut bytes");
            MP4Packet a1 = d.getAudioTrack().nextFrame();
            check(a1.getFrameNo() == 5 && a1.getFileOff() == aBases[1] && a1.getSize() == 6,
                    "rigC remainder from the second chunk");
            assertBytesEqual(a1.getData(), expectedPcmBytes(5, 3, true), "rigC second chunk bytes");
            check(d.getAudioTrack().nextFrame() == null, "rigC audio EOF");
        }
    }

    static void timelineNoEdit() throws Exception {
        File rig = buildRigNoEdit();
        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);
            check(d.getTimescale() == 12, "no-edit common clock LCM(12,6,12)=12");
            check(d.getDuration() == 12, "no-edit duration");
            for (int i = 0; i < 6; i++) {
                MP4Packet p = d.getVideoTrack().nextFrame();
                check(p.getFrameNo() == i && p.getPts() == i * 2 && p.getDuration() == 2, "no-edit video " + i);
            }
            check(d.getVideoTrack().nextFrame() == null, "no-edit video EOF");
            MP4Packet a = d.getAudioTrack().nextFrame();
            check(a.getFrameNo() == 0 && a.getMediaPts() == 0, "no-edit audio starts at frame 0");
        }
    }

    // ------------------------------------------------------------------
    // Group 2: atomic seeking
    // ------------------------------------------------------------------

    public static void seekAtomic() throws Exception {
        File rig = buildRigA();
        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);

            // Read a couple of packets so cursors have moved.
            d.getVideoTrack().nextFrame();
            d.getAudioTrack().nextFrame();
            long vPos = d.getVideoTrack().getPosition();
            long aPos = d.getAudioTrack().getPosition();

            // Negative, at-end and past-end seeks are rejected.
            check(!d.seekMovie(-1), "negative seek rejected");
            check(!d.seekMovie(24), "seek at movie end rejected");
            check(!d.seekMovie(100), "seek past end rejected");
            check(d.getVideoTrack().getPosition() == vPos, "video cursor unchanged after bad seek");
            check(d.getAudioTrack().getPosition() == aPos, "audio cursor unchanged after bad seek");

            // Movie tick 1 -> common tick 2, not on a PCM frame (step 3).
            check(!d.seekMovie(1), "non-integral PCM seek rejected");
            check(d.getVideoTrack().getPosition() == vPos, "video untouched after PCM misalignment");
            check(d.getAudioTrack().getPosition() == aPos, "audio untouched after PCM misalignment");

            // Seek inside a video frame: movie tick 9 -> common 18, frame 7
            // spans common [16,20); the packet keeps all source bytes but is
            // clipped to pts=18, duration=2.
            check(d.seekMovie(9), "in-frame video seek accepted");
            check(d.getVideoTrack().getPosition() == 18, "video position after in-frame seek");
            MP4Packet clipped = d.getVideoTrack().nextFrame();
            check(clipped.getPts() == 18 && clipped.getDuration() == 2, "clipped display interval");
            check(clipped.getFrameNo() == 7 && clipped.getMediaPts() == 7, "clipped packet still frame 7");
            assertBytesEqual(clipped.getData(), expectedVideoBytes(7, videoSampleSize(7)),
                    "clipped packet keeps full frame bytes");

            // Audio at the same point sits inside the empty edit: the remaining
            // blank is preserved as silence.
            MP4Packet silence = d.getAudioTrack().nextFrame();
            check(silence.getFrameNo() == -1 && silence.getPts() == 18 && silence.getDuration() == 6,
                    "remaining empty segment preserved");
            check(d.getAudioTrack().nextFrame().getFrameNo() == 0, "audio continues into next media segment");

            // Seeking into the repeated media segment must not jump back to the
            // first occurrence: movie tick 18 -> common 36, segment 3.
            check(d.seekMovie(18), "seek into repeated segment");
            MP4Packet repeated = d.getVideoTrack().nextFrame();
            check(repeated.getFrameNo() == 6 && repeated.getPts() == 36, "repeated segment frame 6");
            MP4Packet repeatedA = d.getAudioTrack().nextFrame();
            check(repeatedA.getFrameNo() == 3 && repeatedA.getPts() == 36, "repeated audio segment frame 3");

            // Seek to zero and replay: identical timeline again.
            check(d.seekMovie(0), "rewind");
            MP4Packet first = d.getVideoTrack().nextFrame();
            check(first.getFrameNo() == -1 && first.getPts() == 0, "rewind replays the gap");
            MP4Packet firstA = d.getAudioTrack().nextFrame();
            check(firstA.getFrameNo() == 3 && firstA.getPts() == 0, "rewind replays audio");

            // EOF then reposition: packets flow again.
            MP4Packet p;
            while ((p = d.getVideoTrack().nextFrame()) != null)
                ;
            while ((p = d.getAudioTrack().nextFrame()) != null)
                ;
            check(d.getVideoTrack().getPosition() == 48 && d.getAudioTrack().getPosition() == 48,
                    "both tracks parked at duration");
            check(d.seekMovie(12), "seek after EOF");
            MP4Packet recovered = d.getVideoTrack().nextFrame();
            check(recovered.getFrameNo() == 0 && recovered.getPts() == 24, "recovered after EOF reposition");
        }
    }

    // ------------------------------------------------------------------
    // Group 3: IO compatibility, retry and legacy factories
    // ------------------------------------------------------------------

    public static void ioCompat() throws Exception {
        retryAfterFailure();
        shortSourceThrows();
        smallAndInvalidBuffers();
        silenceBounded();
        exampleArtifacts();
        legacyFactories();
    }

    static void runSample(File fixture, File videoOut, File pcmOut) throws Exception {
        // The sample is part of the default package, so invoke it reflectively.
        Class<?> sample = Class.forName("EditedPlayback");
        sample.getMethod("main", String[].class).invoke(null,
                (Object) new String[] { fixture.getPath(), videoOut.getPath(), pcmOut.getPath() });
    }

    static void exampleArtifacts() throws Exception {
        // Drive the sample program exactly like the build entry point and
        // verify this run's artifacts and their byte content.
        File fixture = task_support.Baseline.fixture();
        File videoOut = new File(".task-build/example-video.bin");
        File pcmOut = new File(".task-build/example-pcm.bin");
        java.nio.file.Files.deleteIfExists(videoOut.toPath());
        java.nio.file.Files.deleteIfExists(pcmOut.toPath());
        runSample(fixture, videoOut, pcmOut);

        check(videoOut.isFile() && videoOut.length() > 0, "video artifact produced");
        check(pcmOut.isFile() && pcmOut.length() > 0, "pcm artifact produced");
        // Only frame C survives the video edit list: 43 00 00.
        byte[] videoBytes = java.nio.file.Files.readAllBytes(videoOut.toPath());
        check(Arrays.equals(videoBytes, new byte[] { 0x43, 0, 0 }), "video artifact content");
        // PCM: frames 3,4,5 then three silence frames then frames 1,2.
        byte[] pcmBytes = java.nio.file.Files.readAllBytes(pcmOut.toPath());
        byte[] expectedPcm = new byte[] { 3, 0, 4, 0, 5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0 };
        check(Arrays.equals(pcmBytes, expectedPcm), "pcm artifact content");

        // Re-running must clear and regenerate the outputs rather than append.
        runSample(fixture, videoOut, pcmOut);
        check(java.nio.file.Files.readAllBytes(videoOut.toPath()).length == 3, "video artifact regenerated");
        check(java.nio.file.Files.readAllBytes(pcmOut.toPath()).length == 16, "pcm artifact regenerated");
    }

    static void silenceBounded() throws Exception {
        File rig = buildRigSilence();
        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);
            check(d.getTimescale() == 10000, "silence rig common clock");
            MP4Packet first = d.getAudioTrack().nextFrame();
            check(first.getFrameNo() == -1 && first.getDuration() == 4096 && first.getSize() == 4096 * 2,
                    "first silence packet capped at 4096 frames");
            assertBytesEqual(first.getData(), new byte[4096 * 2], "first silence is all zero");
            MP4Packet second = d.getAudioTrack().nextFrame();
            check(second.getFrameNo() == -1 && second.getDuration() == 904, "remaining silence frames");
            assertBytesEqual(second.getData(), new byte[904 * 2], "second silence is all zero");
            MP4Packet media = d.getAudioTrack().nextFrame();
            check(media.getFrameNo() == 0 && media.getPts() == 5000, "media starts after the gap");
        }
    }

    static void retryAfterFailure() throws Exception {
        File rig = buildRigA();
        FlakyChannel flaky;
        try (SeekableByteChannel raw = NIOUtils.readableChannel(rig)) {
            flaky = new FlakyChannel(raw);
            EditedMP4Demuxer d = EditedMP4Demuxer.create(flaky);
            long[] vBases = chunkBases(rig, 0);

            // First packet is a gap (no IO). Arm failure for the first media
            // read of each track.
            check(d.getVideoTrack().nextFrame().getFrameNo() == -1, "gap needs no channel read");
            flaky.failNextRead();
            ByteBuffer storage = ByteBuffer.allocate(64);
            int posBefore = storage.position();
            int limBefore = storage.limit();
            boolean threw = false;
            try {
                d.getVideoTrack().getNextFrame(storage);
            } catch (IOException e) {
                threw = true;
            }
            check(threw, "video read failure surfaces as IOException");
            check(storage.position() == posBefore && storage.limit() == limBefore, "storage untouched on failure");
            check(d.getVideoTrack().getPosition() == 12, "video logical position unchanged after failure");
            flaky.clearFailure();

            MP4Packet retry = d.getVideoTrack().getNextFrame(storage);
            check(retry.getFrameNo() == 6 && retry.getFileOff() == videoOffsetRigA(vBases, 6),
                    "retry yields the same video packet");
            assertBytesEqual(retry.getData(), expectedVideoBytes(6, videoSampleSize(6)), "retry video bytes");
            check(storage.position() == posBefore && storage.limit() == limBefore,
                    "storage position/limit preserved after read");

            // Same contract for PCM.
            flaky.failNextRead();
            ByteBuffer pcmStorage = ByteBuffer.allocate(32);
            int pcmPos = pcmStorage.position(), pcmlim = pcmStorage.limit();
            threw = false;
            try {
                d.getAudioTrack().getNextFrame(pcmStorage);
            } catch (IOException e) {
                threw = true;
            }
            check(threw, "pcm read failure surfaces as IOException");
            check(pcmStorage.position() == pcmPos && pcmStorage.limit() == pcmlim, "pcm storage untouched");
            check(d.getAudioTrack().getPosition() == 0, "audio logical position unchanged after failure");
            flaky.clearFailure();
            MP4Packet pcmRetry = d.getAudioTrack().getNextFrame(pcmStorage);
            check(pcmRetry.getFrameNo() == 3 && pcmRetry.getSize() == 6, "retry yields the same PCM packet");
            assertBytesEqual(pcmRetry.getData(), expectedPcmBytes(3, 3, false), "retry pcm bytes");
        }
    }

    static void shortSourceThrows() throws Exception {
        File rig = buildRigA();
        try (SeekableByteChannel raw = NIOUtils.readableChannel(rig)) {
            FlakyChannel flaky = new FlakyChannel(raw);
            EditedMP4Demuxer d = EditedMP4Demuxer.create(flaky);
            d.getVideoTrack().nextFrame(); // gap
            long[] vBases = chunkBases(rig, 0);
            long frameSixOff = videoOffsetRigA(vBases, 6);
            // Effective file ends one byte into the first media video frame.
            flaky.truncate((int) (raw.size() - frameSixOff - 1));
            boolean threw = false;
            try {
                d.getVideoTrack().nextFrame();
            } catch (IOException e) {
                threw = true;
            }
            check(threw, "short source raises IOException, not a silent EOF");
            check(d.getVideoTrack().getPosition() == 12, "cursor stays before the failed frame");
        }
    }

    static void smallAndInvalidBuffers() throws Exception {
        File rig = buildRigA();
        try (SeekableByteChannel ch = NIOUtils.readableChannel(rig)) {
            EditedMP4Demuxer d = EditedMP4Demuxer.create(ch);
            d.getVideoTrack().nextFrame(); // gap

            // A PCM buffer holding exactly one frame shrinks multi-frame
            // packets to whole frames.
            ByteBuffer oneFrame = ByteBuffer.allocate(2);
            MP4Packet first = d.getAudioTrack().getNextFrame(oneFrame);
            check(first.getSize() == 2 && first.getDuration() == 3 && first.getFrameNo() == 3,
                    "shrunk to one PCM frame");
            assertBytesEqual(first.getData(), expectedPcmBytes(3, 1, false), "single frame bytes");
            MP4Packet second = d.getAudioTrack().getNextFrame(oneFrame);
            check(second.getFrameNo() == 4, "next call continues at the next frame");
            MP4Packet third = d.getAudioTrack().getNextFrame(oneFrame);
            check(third.getFrameNo() == 5, "still inside the first physical chunk");
            MP4Packet fourth = d.getAudioTrack().getNextFrame(oneFrame);
            check(fourth.getFrameNo() == 6 && fourth.getFileOff() == chunkBases(rig, 1)[1],
                    "crosses into the next physical chunk with a fresh packet");

            // A buffer that cannot hold one PCM frame is an IAE, never an EOF.
            long pos = d.getAudioTrack().getPosition();
            boolean iae = false;
            try {
                d.getAudioTrack().getNextFrame(ByteBuffer.allocate(1));
            } catch (IllegalArgumentException e) {
                iae = true;
            }
            check(iae, "sub-frame PCM storage rejected");
            check(d.getAudioTrack().getPosition() == pos, "audio state unchanged after IAE");

            // Video storage smaller than a whole frame is an IAE too.
            long vpos = d.getVideoTrack().getPosition();
            iae = false;
            try {
                d.getVideoTrack().getNextFrame(ByteBuffer.allocate(1));
            } catch (IllegalArgumentException e) {
                iae = true;
            }
            check(iae, "undersized video storage rejected");
            check(d.getVideoTrack().getPosition() == vpos, "video state unchanged after IAE");

            // nextFrame() allocates internally and still works.
            MP4Packet allocated = d.getVideoTrack().nextFrame();
            check(allocated != null && allocated.getFrameNo() == 6, "nextFrame self-allocates storage");

            // A pre-positioned storage keeps its position/limit and the
            // returned view contains only the bytes written this call.
            check(d.seekMovie(0), "rewind for storage contract check");
            d.getVideoTrack().nextFrame(); // gap
            ByteBuffer prefixed = ByteBuffer.allocate(32);
            prefixed.put(new byte[] { 9, 9, 9, 9 }); // position now 4
            int prePos = prefixed.position();
            int preLim = prefixed.limit();
            MP4Packet intoPrefixed = d.getVideoTrack().getNextFrame(prefixed);
            check(prefixed.position() == prePos && prefixed.limit() == preLim, "prefixed storage untouched");
            assertBytesEqual(intoPrefixed.getData(), expectedVideoBytes(6, videoSampleSize(6)),
                    "prefixed storage returns only this call's bytes");

            ByteBuffer pcmPrefixed = ByteBuffer.allocate(32);
            pcmPrefixed.put(new byte[] { 7, 7 }); // position now 2
            int ppPos = pcmPrefixed.position();
            int ppLim = pcmPrefixed.limit();
            MP4Packet pcmInto = d.getAudioTrack().getNextFrame(pcmPrefixed);
            check(pcmPrefixed.position() == ppPos && pcmPrefixed.limit() == ppLim, "prefixed pcm storage untouched");
            assertBytesEqual(pcmInto.getData(), expectedPcmBytes(3, 3, false),
                    "prefixed pcm returns the full chunk cut, not the buffer prefix");
        }
    }

    static void legacyFactories() throws Exception {
        // The ordinary and raw demuxer factories keep their physical-stream
        // behaviour on the same edited file.
        File fixture = task_support.Baseline.fixture();
        for (boolean raw : new boolean[] { false, true }) {
            try (SeekableByteChannel ch = NIOUtils.readableChannel(fixture)) {
                MP4Demuxer demux = raw ? MP4Demuxer.createRawMP4Demuxer(ch) : MP4Demuxer.createMP4Demuxer(ch);
                StringBuilder letters = new StringBuilder();
                Packet vp;
                while ((vp = demux.getVideoTrack().nextFrame()) != null) {
                    ByteBuffer b = vp.getData();
                    check(b.remaining() == 3, "legacy video frame size");
                    letters.append((char) b.get());
                }
                check(letters.toString().equals("ABCD"), "legacy video payload " + raw);

                org.jcodec.common.SeekableDemuxerTrack audio =
                        (org.jcodec.common.SeekableDemuxerTrack) demux.getAudioTracks().get(0);
                StringBuilder words = new StringBuilder();
                Packet ap;
                while ((ap = audio.nextFrame()) != null) {
                    ByteBuffer b = ap.getData();
                    while (b.hasRemaining()) {
                        int x = b.get() & 255;
                        check(b.get() == 0, "legacy PCM order");
                        if (words.length() > 0)
                            words.append(',');
                        words.append(x);
                    }
                }
                check(words.toString().equals("1,2,3,4,5,6,7,8"), "legacy PCM payload " + raw);
                check(audio.gotoFrame(2), "legacy PCM gotoFrame");
                MP4Packet seeked = (MP4Packet) audio.nextFrame();
                check(seeked.getData().get() == 3, "legacy chunk-internal seek " + raw);
            }
        }
    }
}
