package task_support;
import java.util.*;

public final class Checks {
    private static final Set<String> groups = new LinkedHashSet<>();
    private static int assertions;
    private static boolean inside;
    public static void check(boolean condition, String detail) {
        if (!inside) throw new AssertionError("check outside group");
        if (!condition) throw new AssertionError(detail);
        assertions++;
    }
    public static void group(String name, Runnable body) {
        if (inside || groups.contains(name)) throw new AssertionError("duplicate/nested group");
        int before=assertions;
        inside=true;
        try { body.run(); } finally { inside=false; }
        if (assertions==before) throw new AssertionError("empty group: "+name);
        groups.add(name);
        System.out.println(name+"_OK assertions="+(assertions-before));
    }
    static void finish() {
        if (!groups.equals(new LinkedHashSet<>(Arrays.asList("ENTITIES","LAYOUT","READER","DOM"))))
            throw new AssertionError("required groups: "+groups);
    }
}
