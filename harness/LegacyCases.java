import java.lang.reflect.*;
import org.junit.Ignore;
import org.junit.Test;
/** Executes these exact original methods, not a rewritten assertion list. */
public final class LegacyCases {
    public static void main(String[] args) throws Exception {
        String[] names = {"parsesSimpleDocument", "handlesNestedImplicitTable", "handlesTbodyTable",
            "handlesMisnestedTagsBI", "handlesMisnestedTagsBP", "handlesUnexpectedMarkupInTables",
            "reconstructFormattingElementsInTable", "createsFormElements",
            "associatedFormControlsWithDisjointForms", "handlesInputInTable", "testFragment",
            "testTemplateInsideTable"};
        int count = 0;
        for (String name : names) {
            run(org.jsoup.parser.HtmlParserTest.class, name);
            count++;
        }
        run(org.jsoup.parser.HtmlTreeBuilderTest.class, "ensureSearchArraysAreSorted");
        System.out.println("LEGACY_PARSER_OK cases=" + (count + 1));
    }
    private static void run(Class<?> type, String name) throws Exception {
        Method m = type.getMethod(name);
        if (!m.isAnnotationPresent(Test.class) || m.isAnnotationPresent(Ignore.class))
            throw new AssertionError("not a runnable original test " + name);
        try { m.invoke(type.newInstance()); }
        catch (InvocationTargetException e) { throw new AssertionError("legacy " + name, e.getCause()); }
        System.out.println("LEGACY_PASS " + name);
    }
}
