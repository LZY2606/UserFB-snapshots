import java.lang.reflect.*;
import java.util.*;
public class CaseHarness {
 private static int assertions;
 public static void check(boolean b,String m){assertions++;if(!b)throw new AssertionError(m);}
 public static void equal(Object want,Object got){assertions++;if(!Objects.equals(want,got))throw new AssertionError("expected="+want+" actual="+got);}
 public static void main(String[] a)throws Exception{
  Method[] all=Class.forName(a[0]).getDeclaredMethods();Arrays.sort(all,Comparator.comparing(Method::getName));int count=0;
  for(Method m:all)if(m.getName().startsWith(a[1])){
   if(!Modifier.isPublic(m.getModifiers())||!Modifier.isStatic(m.getModifiers())||m.getParameterCount()!=0||m.getReturnType()!=void.class)throw new AssertionError("test must be public static void noarg: "+m);
   assertions=0;try{m.invoke(null);}catch(InvocationTargetException e){throw new AssertionError(m.getName(),e.getCause());}
   if(assertions==0)throw new AssertionError("no harness assertions: "+m.getName());count++;System.out.println("PASS "+m.getName());
  }
  if(count<3)throw new AssertionError("need >=3 distinct tests; got "+count);
 }
}
