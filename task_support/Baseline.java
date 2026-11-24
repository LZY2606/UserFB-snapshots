import bdsup2sub.supstream.bd.SupBD;
public final class Baseline {
 public static void main(String[] args) throws Exception {
  SupBD s = new SupBD("src/test/resources/test.sup");
  if(s.getFrameCount()!=1) throw new AssertionError("frame count");
  s.decode(0);
  int width=s.getBitmap().getWidth(), height=s.getBitmap().getHeight();
  if(width!=1920 || height!=125) throw new AssertionError("bitmap");
  long visible=java.util.Arrays.stream(s.getBitmap().toARGB(s.getPalette())).filter(x->(x>>>24)>0).count();
  if(visible==0) throw new AssertionError("empty image");
  System.out.println("BASELINE_PGS frames="+s.getFrameCount()+" bitmap="+width+"x"+height+" visible="+visible+" start="+s.getStartTime(0)+" end="+s.getEndTime(0));
  s.close();
  if(args.length==0) {
   SupBD dual = new SupBD("task_support/fixtures/two-objects.sup");
   dual.decode(0);
   System.out.println("DUAL_OBJECT_OBSERVED frames="+dual.getFrameCount()+" bitmap="+dual.getBitmap().getWidth()+"x"+dual.getBitmap().getHeight()+" offset="+dual.getSubPicture(0).getXOffset());
   dual.close();
  }
 }
}
