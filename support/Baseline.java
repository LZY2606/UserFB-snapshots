import org.jsoup.Jsoup;import org.jsoup.nodes.*;import org.jsoup.parser.*;
public class Baseline {
 public static void main(String[] a)throws Exception{
  LegacySmoke.run();Document d=Jsoup.parse("<div id=host><template shadowrootmode=open><b id=inside>hidden</b></template><i>light</i></div>");
  CaseHarness.equal(1,d.select("#inside").size());boolean api=false;for(java.lang.reflect.Method m:Parser.class.getMethods())if(m.getName().equals("setDeclarativeShadowRoots"))api=true;
  CaseHarness.check(!api,"feature unexpectedly exists");System.out.println("ordinary-template=1 light-query-inside="+d.select("#inside").size()+" shadow-api="+api);
 }
}
