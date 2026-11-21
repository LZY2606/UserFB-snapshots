package task_support;
import java.io.File;
import org.jcodec.common.io.NIOUtils;
import org.jcodec.common.io.SeekableByteChannel;
import org.jcodec.containers.mp4.demuxer.EditedMP4Demuxer;
public final class Binding {
  public static void main(String[] args) throws Exception {
    try(SeekableByteChannel ch=NIOUtils.readableChannel(new File(".task-build/edited-input.mp4"))) {
      EditedMP4Demuxer session=EditedMP4Demuxer.create(ch);
      if(session==null||session.getTimescale()!=24||session.getDuration()!=24)throw new AssertionError("session common clock");
      if(!ch.isOpen())throw new AssertionError("caller channel closed");
      System.out.println("PRODUCTION_BINDING common_scale=24 duration=24");
    }
  }
}
