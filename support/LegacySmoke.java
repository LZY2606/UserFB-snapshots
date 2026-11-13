import org.jsoup.Jsoup;
import org.jsoup.nodes.*;
import org.jsoup.parser.*;
import java.io.*;
public class LegacySmoke {
 public static void run()throws Exception{
  Document d=Jsoup.parse("<div id=a><p>中 &amp; 文</p></div>");
  CaseHarness.equal("中 & 文",d.selectFirst("#a p").text());
  Document t=Jsoup.parse("<table><tr><td>x</td></tr></table>");
  CaseHarness.equal("x",t.selectFirst("table > tbody > tr > td").text());
  Document c=Jsoup.parse("<div><template><b>stored</b></template><p>light</p></div>");
  CaseHarness.equal(1,c.select("template > b").size());
  try(StreamParser s=new StreamParser(Parser.htmlParser())){s.parse(new StringReader("<p id=a>A</p><p id=b>B</p>"),"");CaseHarness.equal("a",s.selectNext("p").id());CaseHarness.equal("b",s.selectNext("p").id());}
 }
 public static void main(String[] a)throws Exception{run();System.out.println("LEGACY_SMOKE_OK");}
}
