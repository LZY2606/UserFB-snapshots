import org.jsoup.Jsoup;
import org.jsoup.nodes.Document;
import org.jsoup.nodes.Element;
import org.jsoup.nodes.FormElement;
import org.jsoup.select.Elements;

/**
 An example of the new form ownership API: parse a query form, then copy the page and adjust the query conditions
 before reading {@link FormElement#formData()} or submitting. Run with:
 <pre>javac -cp .task-build/classes FormOwnerExample.java &amp;&amp; java -cp .:... FormOwnerExample</pre>
 */
public class FormOwnerExample {
    public static void main(String[] args) {
        Document query = Jsoup.parse(
            "<table><tr><form id=search action=/search>" +
                "<input type=hidden id=token name=token value=abc>" +
                "<td><input id=q name=q value=jsoup></td>" +
                "</tr></table>",
            "http://localhost/query");

        FormElement form = (FormElement) query.getElementById("search");
        // The hidden token was foster-parented out of the form; it is still associated via the parser's link.
        Element token = query.getElementById("token");
        System.out.println("token owner: " + token.formOwner().id());
        System.out.println("initial data: " + form.formData());

        // Copy the page so we can adjust the query without disturbing the original.
        Document copy = query.clone();
        FormElement copyForm = (FormElement) copy.getElementById("search");
        Element copyQ = copy.getElementById("q");
        copyQ.attr("value", "form ownership");

        // Show each control resolving to the copy's form, never the original's.
        for (Element control : copyForm.elements()) {
            Element owner = control.formOwner();
            System.out.println("copy control " + control.id() + " -> " + (owner == null ? "none" : owner.id()));
        }

        System.out.println("copy data: " + copyForm.formData());
        System.out.println("copy submits: " + copyForm.submit().request().data());
        System.out.println("original data unchanged: " + form.formData());

        // Move a control's container into another form; ownership follows the current tree.
        Document moved = Jsoup.parse(
            "<form id=a><div id=box><input id=x name=x value=1></div></form><form id=b></form>",
            "http://localhost/");
        FormElement a = (FormElement) moved.getElementById("a");
        FormElement b = (FormElement) moved.getElementById("b");
        Element x = moved.getElementById("x");
        b.appendChild(moved.getElementById("box"));
        System.out.println("after move: x owner=" + x.formOwner().id()
            + " a submits=" + a.formData() + " b submits=" + b.formData());

        // The returned list is a snapshot: mutating it does not change the DOM or the form's associations.
        Elements controls = b.elements();
        controls.clear();
        System.out.println("list is independent, b still has " + b.elements().size() + " control(s): " + b.formData());

        // A detached control does not keep its old form, even if reinserted elsewhere.
        x.remove();
        System.out.println("after remove: x owner=" + x.formOwner() + " b submits=" + b.formData());
    }
}
