import org.jsoup.Jsoup;
import org.jsoup.nodes.*;
import java.util.*;
public final class StartingPoint {
  private static void require(boolean v) { if (!v) throw new AssertionError(); }
  public static void main(String[] args) {
    Document d = Jsoup.parse("<table><tr><form id=f><input type=hidden id=a name=a value=1><td><input id=b name=b value=2></td></tr></table>", "http://localhost/query");
    FormElement f = (FormElement)d.getElementById("f");
    require(f.formData().size()==2);
    require(f.elements().get(1)==d.getElementById("b"));
    require(!f.getAllElements().contains(d.getElementById("b")));
    System.out.println("NORMAL_OK parser-linked=a,b values=1,2");
    Document copy=d.clone();
    FormElement cf=(FormElement)copy.getElementById("f");
    int originals=0;
    for(Element el:cf.elements()) if(el.ownerDocument()==d) originals++;
    System.out.println("CLONE_ORIGINAL_CONTROLS="+originals);
    Document m=Jsoup.parse("<form id=x><div id=box><input id=q name=q value=v></div></form><form id=y></form>");
    FormElement x=(FormElement)m.getElementById("x"), y=(FormElement)m.getElementById("y");
    y.appendChild(m.getElementById("box"));
    System.out.println("MOVED_OLD_OWNER="+x.elements().contains(m.getElementById("q")));
    System.out.println("MOVED_NEW_OWNER="+y.elements().contains(m.getElementById("q")));
  }
}
