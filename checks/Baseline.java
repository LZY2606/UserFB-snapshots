import sonic.Sonic;
import java.lang.reflect.Method;
public class Baseline {
  public static void main(String[] args) throws Exception {
    short[] mono=new short[4096],stereo=new short[8192],out=new short[16000];
    for(int i=0;i<4096;i++){mono[i]=(short)Math.round(10000*Math.sin(i*2*Math.PI*220/48000));stereo[2*i]=mono[i];stereo[2*i+1]=(short)-mono[i];}
    Sonic m=new Sonic(48000,1),s=new Sonic(48000,2);m.setQuality(1);s.setQuality(1);
    Method find=Sonic.class.getDeclaredMethod("findPitchPeriod",short[].class,int.class,boolean.class);find.setAccessible(true);
    int pm=(int)find.invoke(m,mono,0,true),ps=(int)find.invoke(s,stereo,0,true);
    System.out.printf("JAVA_PERIOD_OBSERVED mono=%d antiphase=%d%n",pm,ps);
    if(Math.abs(pm-218)>1)throw new AssertionError("mono period");
    if(args.length>0 && Math.abs(pm-ps)>1)throw new AssertionError("ANTIPHASE_PERIOD_MISMATCH");
    s.setSpeed(1.5f);s.writeShortToStream(stereo,4096);s.flushStream();int n=s.readShortFromStream(out,8000);
    if(n<=1000 || n>=4096)throw new AssertionError("output length");
    for(int i=0;i<n;i++)if(Math.abs(out[2*i]+out[2*i+1])>1)throw new AssertionError("channel relation");
    System.out.printf("SONIC_JAVA_PUBLIC_PATH_OK frames=%d%n",n);
  }
}
