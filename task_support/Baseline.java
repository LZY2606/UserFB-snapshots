package task_support;
import java.io.File;
import java.nio.ByteBuffer;
import java.util.Arrays;
import org.jcodec.common.AudioFormat;
import org.jcodec.common.DemuxerTrack;
import org.jcodec.common.SeekableDemuxerTrack;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.common.model.Packet;
import org.jcodec.common.model.Packet.FrameType;
import org.jcodec.common.model.Rational;
import org.jcodec.common.model.Size;
import org.jcodec.containers.mp4.MP4Packet;
import org.jcodec.containers.mp4.MP4TrackType;
import org.jcodec.containers.mp4.boxes.Edit;
import org.jcodec.containers.mp4.boxes.MovieBox;
import org.jcodec.containers.mp4.boxes.TrakBox;
import org.jcodec.containers.mp4.boxes.VideoSampleEntry;
import org.jcodec.containers.mp4.demuxer.MP4Demuxer;
import org.jcodec.containers.mp4.muxer.MP4Muxer;
import org.jcodec.containers.mp4.muxer.MP4MuxerTrack;
import org.jcodec.containers.mp4.muxer.PCMMP4MuxerTrack;
import org.jcodec.containers.mp4.muxer.AbstractMP4MuxerTrack;
import org.jcodec.common.model.Unit;

public final class Baseline {
  private static void require(boolean b,String s){if(!b)throw new AssertionError(s);}
  public static File fixture() throws Exception {
    File file=new File(".task-build/edited-input.mp4");
    java.nio.file.Files.deleteIfExists(file.toPath());
    try(SeekableByteChannel out=NIOUtils.writableChannel(file)) {
      MP4Muxer mux=MP4Muxer.createMP4MuxerToChannel(out);
      MP4MuxerTrack v=mux.addTrack(new MP4MuxerTrack(1,MP4TrackType.VIDEO));
      v.addSampleEntry(VideoSampleEntry.videoSampleEntry("mp4v",new Size(1,1),"opaque MP4V samples"));
      for(int i=0;i<4;i++)v.addFrame(Packet.createPacket(ByteBuffer.wrap(new byte[]{(byte)('A'+i),0,0}),i,4,1,i,FrameType.KEY,null));
      PCMMP4MuxerTrack a=mux.addPCMAudioTrack(new AudioFormat(8,16,1,true,false));
      a.setTgtChunkDuration(new Rational(1,4),Unit.SEC);
      int index=1;
      for(int n:new int[]{3,2,3}) {
        ByteBuffer b=ByteBuffer.allocate(n*2);
        for(int j=0;j<n;j++)b.put((byte)index++).put((byte)0);
        b.flip(); a.addSamples(b);
      }
      MovieBox movie=mux.finalizeHeader();
      movie.getMovieHeader().setTimescale(12); movie.setDuration(12);
      TrakBox[] tracks=movie.getTracks();
      tracks[0].setEdits(Arrays.asList(new Edit(3,-1,1),new Edit(3,2,1),new Edit(3,0,1),new Edit(3,2,1)));
      tracks[1].setEdits(Arrays.asList(new Edit(3,2,1),new Edit(3,-1,1),new Edit(3,0,1),new Edit(3,2,1)));
      tracks[0].setDuration(12);tracks[1].setDuration(12);
      mux.storeHeader(movie);
    }
    return file;
  }
  static String observe(File file,boolean raw) throws Exception {
    try(SeekableByteChannel ch=NIOUtils.readableChannel(file)) {
      MP4Demuxer d=raw?MP4Demuxer.createRawMP4Demuxer(ch):MP4Demuxer.createMP4Demuxer(ch);
      DemuxerTrack video=d.getVideoTrack();
      SeekableDemuxerTrack audio=(SeekableDemuxerTrack)d.getAudioTracks().get(0);
      int videoCount=0,audioCount=0;StringBuilder letters=new StringBuilder(),words=new StringBuilder();
      for(Packet p;(p=video.nextFrame())!=null;){ByteBuffer b=p.getData();require(b.remaining()==3,"video size");letters.append((char)b.get());videoCount++;}
      for(Packet p;(p=audio.nextFrame())!=null;){ByteBuffer b=p.getData();while(b.hasRemaining()){int x=b.get()&255;require(b.get()==0,"PCM byte order");if(words.length()>0)words.append(',');words.append(x);audioCount++;}}
      require(videoCount==4&&letters.toString().equals("ABCD"),"normal physical video");
      require(audioCount==8&&words.toString().equals("1,2,3,4,5,6,7,8"),"normal physical PCM");
      require(audio.gotoFrame(2),"native PCM seek");
      MP4Packet p=(MP4Packet)audio.nextFrame();require(p.getData().remaining()==2&&p.getData().get()==3,"chunk-internal source frame");
      return "video="+letters+" PCM="+words+" physical_frames="+videoCount+"/"+audioCount;
    }
  }
  public static void main(String[] args) throws Exception {
    File file=fixture();
    String normal=observe(file,false),raw=observe(file,true);
    require(normal.equals(raw),"old normal/raw parity");
    System.out.println("NORMAL "+normal+" normal_raw_equal=true");
    if(args.length>0&&args[0].equals("--observe-gap"))
      System.out.println("GAP old_stream=ABCD/1,2,3,4,5,6,7,8 edited_stream=gap,C,A,C/3,4,0,0,1,2,3,4 movie_scale=12 video_scale=4 pcm_scale=8");
  }
}
