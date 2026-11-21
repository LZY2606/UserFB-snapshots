package task_support;
import java.lang.reflect.*;
public final class Driver {
  public static void main(String[] args) throws Exception {
    Class<?> test=Class.forName("org.jcodec.containers.mp4.demuxer.EditedPlaybackTest");
    for(String group:new String[]{"timeline","seekAtomic","ioCompat"}) {
      Check.checks=0; Method m=test.getMethod(group);
      if(m.getReturnType()!=void.class||!Modifier.isStatic(m.getModifiers()))throw new AssertionError("expected static void "+group);
      m.invoke(null);
      if(Check.checks<1)throw new AssertionError("no checks "+group);
      System.out.println("GROUP "+group+" checks="+Check.checks+" COMPLETE");
    }
    System.out.println("TARGET_COMPLETE groups=3");
  }
}
