package org.jooq.lambda.support;
import java.util.*;
public final class UFHarness {
 public interface Action {void run() throws Exception;}
 static class Test {String name,group;Action fn;Test(String n,String g,Action f){name=n;group=g;fn=f;}}
 public static final class Registry {
  final List<Test> tests=new ArrayList<>();
  public void add(String name,String group,Action fn){tests.add(new Test(name,group,fn));}
 }
 private static long checks=0;
 public static void check(boolean value){checks++;if(!value)throw new AssertionError("contract content assertion");}
 public static void main(String[] args) throws Exception {
  Registry r=new Registry();Class.forName("org.jooq.lambda.SpillSortContract").getMethod("register",Registry.class).invoke(null,r);
  if(r.tests.size()<6)throw new AssertionError("need six cases");
  Set<String> names=new HashSet<>();int total=0;
  for(String group:Arrays.asList("normal","boundary","integration")){
   int count=0;long before=checks;
   for(Test t:r.tests)if(group.equals(t.group)){
    if(!names.add(t.name)||t.fn==null)throw new AssertionError("invalid registration");
    long start=checks;t.fn.run();if(checks==start)throw new AssertionError("zero assertions");
    count++;total++;System.out.println("CASE "+t.name+" PASS");
   }
   if(count==0||checks==before)throw new AssertionError("empty group");
   System.out.println("GROUP "+group+" PASS cases="+count+" checks="+(checks-before));
  }
  if(total!=r.tests.size())throw new AssertionError("unexecuted cases");
  System.out.println("UNIT_TARGET_COMPLETE cases="+total+" checks="+checks);
 }
}
