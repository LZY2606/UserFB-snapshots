import bdsup2sub.supstream.bd.SupBD;
import bdsup2sub.bitmap.Bitmap;

public final class Verify {
 public static void main(String[] a) throws Exception {
  SupBD s = new SupBD("task_support/fixtures/two-objects.sup");
  s.decode(0);
  Bitmap bm = s.getBitmap();
  StringBuilder sb = new StringBuilder();
  sb.append("size=").append(bm.getWidth()).append("x").append(bm.getHeight()).append(" pixels=");
  for (byte v : bm.getInternalBuffer()) sb.append(v & 0xff).append(' ');
  System.out.println(sb.toString().trim());
  System.out.println("offset=(" + s.getSubPicture(0).getXOffset() + "," + s.getSubPicture(0).getYOffset() + ")");
  System.out.println("time=" + s.getStartTime(0) + ".." + s.getEndTime(0) + " forced=" + s.isForced(0));
  s.close();
 }
}
