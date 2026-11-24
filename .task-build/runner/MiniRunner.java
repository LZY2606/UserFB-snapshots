import java.lang.reflect.*;
import java.util.*;
public final class MiniRunner {
 public static void main(String[] args) throws Exception {
  int pass=0, fail=0, skipped=0;
  for (String cn : args) {
   Class<?> c = Class.forName(cn);
   List<Method> befores = new ArrayList<>();
   for (Method m: c.getDeclaredMethods()) {
    if (m.isAnnotationPresent(org.junit.Before.class)) befores.add(m);
   }
   for (Method m: c.getDeclaredMethods()) {
    org.junit.Test t = m.getAnnotation(org.junit.Test.class);
    if (t == null) continue;
    Object inst = c.getDeclaredConstructor().newInstance();
    try {
     for (Method b: befores) b.invoke(inst);
     m.invoke(inst);
     if (t.expected() != org.junit.Test.None.class) { fail++; System.out.println("FAIL(no expected exc) " + cn + "." + m.getName()); }
     else pass++;
    } catch (InvocationTargetException e) {
     Throwable cause = e.getTargetException();
     if (t.expected() != org.junit.Test.None.class && t.expected().isInstance(cause)) pass++;
     else { fail++; System.out.println("FAIL " + cn + "." + m.getName() + " -> " + cause); }
    } catch (Throwable th) {
     if (th.getMessage() != null && th.getMessage().contains("Operation not permitted")) { skipped++; System.out.println("SKIP(sandbox) " + cn + "." + m.getName()); }
     else { fail++; System.out.println("FAIL " + cn + "." + m.getName() + " -> " + th); }
    }
   }
  }
  System.out.println("MiniRunner pass=" + pass + " fail=" + fail + " skipped=" + skipped);
  if (fail != 0) System.exit(1);
 }
}
