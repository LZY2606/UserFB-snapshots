package task_support;

import java.util.LinkedHashMap;
import java.util.Map;

public final class Checks {
    private final Map<String,Integer> counts = new LinkedHashMap<>();
    private String active;
    public void group(String name) {
        if (!name.equals("graph") && !name.equals("parallel") && !name.equals("cleanup")) throw new AssertionError("unknown group: " + name);
        if (counts.containsKey(name)) throw new AssertionError("duplicate group: " + name);
        active = name;
        counts.put(name, 0);
    }
    public void check(boolean condition, String message) {
        if (active == null) throw new AssertionError("group required");
        if (!condition) throw new AssertionError(message);
        counts.put(active, counts.get(active) + 1);
    }
    public void finish() {
        for (String name : new String[]{"graph", "parallel", "cleanup"}) {
            if (counts.getOrDefault(name, 0) < 2) throw new AssertionError("missing assertions: " + name);
            System.out.println("CHECKED " + name + "=" + counts.get(name));
        }
        System.out.println("WALK_TARGET_COMPLETE");
    }
}
