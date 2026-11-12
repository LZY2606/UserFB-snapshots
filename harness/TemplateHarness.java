import java.lang.annotation.*;
import java.lang.reflect.*;
import java.util.*;
/** Fixed test discovery and assertion accounting. The implementation and new tests are absent. */
public final class TemplateHarness {
    @Retention(RetentionPolicy.RUNTIME) @Target(ElementType.METHOD)
    public @interface Case { String group(); }
    private static int assertions;
    public static void expect(boolean ok, String message) {
        assertions++;
        if (!ok) throw new AssertionError(message);
    }
    public static void equal(Object expected, Object actual, String message) {
        expect(Objects.equals(expected, actual), message + " expected=" + expected + " actual=" + actual);
    }
    public static void main(String[] args) throws Exception {
        Class<?> tests = Class.forName("TemplateParsingTest");
        Set<String> required = new TreeSet<>(Arrays.asList("modes", "boundaries", "fragments"));
        Map<String,Integer> counts = new TreeMap<>();
        Method[] methods = tests.getDeclaredMethods();
        Arrays.sort(methods, Comparator.comparing(Method::getName));
        int cases = 0;
        for (Method m : methods) {
            Case c = m.getAnnotation(Case.class);
            if (c == null) continue;
            if (!required.contains(c.group()) || m.getParameterTypes().length != 0 || m.getReturnType() != void.class
                || !Modifier.isPublic(m.getModifiers()) || !Modifier.isStatic(m.getModifiers()))
                throw new AssertionError("invalid case signature " + m);
            int before = assertions;
            try { m.invoke(null); }
            catch (InvocationTargetException e) { throw new AssertionError("target " + m.getName(), e.getCause()); }
            if (assertions == before) throw new AssertionError("case ran no checks " + m.getName());
            counts.put(c.group(), counts.getOrDefault(c.group(),0) + 1);
            cases++;
            System.out.println("TEMPLATE_PASS " + c.group() + " " + m.getName());
        }
        if (!counts.keySet().equals(required)) throw new AssertionError("missing groups " + counts);
        for (String group : required) System.out.println("TEMPLATE_GROUP " + group + " cases=" + counts.get(group));
        System.out.println("TEMPLATE_CONTRACT_OK cases=" + cases + " groups=3");
    }
}
