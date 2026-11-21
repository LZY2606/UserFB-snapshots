import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;

import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.containers.mp4.MP4Packet;
import org.jcodec.containers.mp4.demuxer.EditedMP4Demuxer;

/**
 * Edit-aware MP4 export demo.
 *
 * Usage: EditedPlayback <input.mp4> <video-out.bin> <pcm-out.bin>
 *
 * Opens the movie with {@link EditedMP4Demuxer} and dumps the actual payload
 * bytes produced by the edited session: every video packet's frame bytes go to
 * the video output (empty edit gaps contribute no bytes), every PCM packet's
 * bytes, including generated silence, go to the PCM output. The input channel
 * is owned by this program and closed when done.
 */
public final class EditedPlayback {

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            System.err.println("Usage: EditedPlayback <input.mp4> <video-out.bin> <pcm-out.bin>");
            System.exit(2);
        }
        File input = new File(args[0]);
        File videoOut = new File(args[1]);
        File pcmOut = new File(args[2]);
        // Fresh artifacts every run.
        deleteIfPresent(videoOut);
        deleteIfPresent(pcmOut);

        try (SeekableByteChannel channel = NIOUtils.readableChannel(input)) {
            EditedMP4Demuxer session = EditedMP4Demuxer.create(channel);
            System.out.println("Opened " + input + " common timescale=" + session.getTimescale()
                    + " duration=" + session.getDuration());

            long videoBytes;
            long pcmBytes;
            try (OutputStream video = new FileOutputStream(videoOut);
                    OutputStream pcm = new FileOutputStream(pcmOut)) {
                videoBytes = dumpVideo(session, video);
                pcmBytes = dumpAudio(session, pcm);
            }
            System.out.println("Video packets -> " + videoOut + " (" + videoBytes + " payload bytes)");
            System.out.println("PCM packets   -> " + pcmOut + " (" + pcmBytes + " payload bytes)");
        }
    }

    private static long dumpVideo(EditedMP4Demuxer session, OutputStream out) throws Exception {
        long written = 0;
        MP4Packet packet;
        while ((packet = session.getVideoTrack().nextFrame()) != null) {
            ByteBuffer data = packet.getData();
            written += writeRemaining(out, data);
        }
        return written;
    }

    private static long dumpAudio(EditedMP4Demuxer session, OutputStream out) throws Exception {
        long written = 0;
        MP4Packet packet;
        while ((packet = session.getAudioTrack().nextFrame()) != null) {
            ByteBuffer data = packet.getData();
            written += writeRemaining(out, data);
        }
        return written;
    }

    private static int writeRemaining(OutputStream out, ByteBuffer data) throws Exception {
        int size = data.remaining();
        if (data.hasArray()) {
            out.write(data.array(), data.arrayOffset() + data.position(), size);
        } else {
            byte[] copy = new byte[size];
            data.get(copy);
            out.write(copy);
        }
        return size;
    }

    private static void deleteIfPresent(File file) throws Exception {
        if (file.exists() && !file.delete())
            throw new IllegalStateException("Cannot overwrite " + file);
    }

    private EditedPlayback() {
    }
}
