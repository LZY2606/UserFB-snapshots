import org.jsoup.Jsoup;
import org.jsoup.nodes.*;
import java.util.Objects;
/** Baseline observes the old behavior; it deliberately does not enforce the future feature. */
public final class StartingPoint {
    private static void eq(Object a, Object b) { if (!Objects.equals(a,b)) throw new AssertionError(a + " != " + b); }
    private static String compact(Document d) { d.outputSettings().prettyPrint(false); return d.outerHtml(); }
    public static void main(String[] args) {
        Document normal = Jsoup.parse("<table id=live><tr><td>A</table><form id=f><input id=x></form>");
        eq(1, normal.select("#live > tbody > tr > td").size());
        eq("A", normal.select("td").text());
        eq("x", ((FormElement)normal.getElementById("f")).elements().first().id());
        System.out.println("NORMAL_TABLE_FORM_OK");
        Document row = Jsoup.parse("<head><template id=t><tr id=r><td id=c>draft</td></tr></template></head><body><p id=live>live</p>");
        eq(0, row.select("#t tr").size());
        eq(0, row.select("#t td").size());
        System.out.println("OLD_TEMPLATE_ROW_LOST rows=" + row.select("#t tr").size() + " cells=" + row.select("#t td").size());
        System.out.println("OLD_ROW_TREE " + compact(row));
        Document forms = Jsoup.parse("<body><form id=outer><input id=a><template id=t><form id=inner><input id=b></form></template><input id=c></form>");
        eq(null, forms.getElementById("inner"));
        eq(java.util.Arrays.asList("a", "b"), ((FormElement)forms.getElementById("outer")).elements().eachAttr("id"));
        eq(forms.body(), forms.getElementById("c").parent());
        System.out.println("OLD_FORM_TREE " + compact(forms));
        System.out.println("OLD_FORM_CONTROLS " + ((FormElement)forms.getElementById("outer")).elements().eachAttr("id"));
        System.out.println("JSOUP_TEMPLATE_BASELINE_OK");
    }
}
