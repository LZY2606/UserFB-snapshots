package task_support;
import be.tarsos.dsp.AudioEvent;
import be.tarsos.dsp.PitchShifter;
import be.tarsos.dsp.io.TarsosDSPAudioFormat;
import java.security.MessageDigest;
public final class Baseline {
  private static final int N=1024,H=256;
  public static float[] render(double factor) {
    PitchShifter p=new PitchShifter(factor,48000,N,N-H);
    TarsosDSPAudioFormat f=new TarsosDSPAudioFormat(48000,16,1,true,false);
    float[] result=new float[96*H];
    int r=0;
    for(int m=0;m<96;m++) {
      AudioEvent event=new AudioEvent(f); float[] input=new float[N];
      for(int i=0;i<N;i++) {
        int t=m*H+i;
        input[i]=(float)(0.008*Math.sin(2*Math.PI*12*t/N)+0.004*Math.cos(2*Math.PI*40*t/N)+0.0005*Math.sin(0.000003*t*t));
      }
      event.setFloatBuffer(input);event.setOverlap(N-H);event.setBytesProcessed((long)m*H*2);
      if(!p.process(event))throw new AssertionError("processor stopped");
      float[] out=event.getFloatBuffer();
      for(int i=N-H;i<N;i++)result[r++]=out[i];
    }
    p.processingFinished();return result;
  }
  public static void main(String[] args)throws Exception {
    for(double factor:new double[]{1.0,0.75,1.5}) {
      float[] output=render(factor);double energy=0;MessageDigest md=MessageDigest.getInstance("SHA-256");
      for(float x:output) {
        if(!Float.isFinite(x))throw new AssertionError("nonfinite");energy+=(double)x*x;
        int b=Float.floatToIntBits(x);
        for(int j=0;j<4;j++)md.update((byte)(b>>>(j*8)));
      }
      if(energy<1e-6)throw new AssertionError("silent");
      StringBuilder hex=new StringBuilder();for(byte b:md.digest())hex.append(String.format("%02x",b&255));
      System.out.printf(java.util.Locale.ROOT,"LEGACY factor=%.2f frames=%d energy=%.9f sha256=%s%n",factor,output.length,energy,hex);
    }
    System.out.println("BASELINE_OK production=PitchShifter hops=288");
  }
}
