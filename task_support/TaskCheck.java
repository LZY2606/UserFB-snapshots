package task_support;
import java.util.*;
public final class TaskCheck {
  @FunctionalInterface public interface Body { void run() throws Exception; }
  private static final class Case {
    final String group,name; final Body body;
    Case(String g,String n,Body b){group=g;name=n;body=b;}
  }
  private static final List<Case> cases=new ArrayList<>();
  private static int checks,failures;
  public static void add(String group,String name,Body body) {
    if(group==null||name==null||body==null)throw new IllegalArgumentException("invalid case");
    cases.add(new Case(group,name,body));
  }
  public static void check(boolean value,String message) {
    checks++;if(!value){failures++;throw new AssertionError(message);}
  }
  public static void main(String[] args)throws Exception {
    Class.forName("task_tests.LinkedPitchTest").getMethod("register").invoke(null);
    if(cases.isEmpty()||cases.size()>10000)throw new AssertionError("zero/too many cases");
    String[] groups={"phase","stream","boundary","legacy"};
    Set<String> allowed=new HashSet<>(Arrays.asList(groups)),names=new HashSet<>();
    for(Case c:cases)if(!allowed.contains(c.group)||!names.add(c.group+"/"+c.name))throw new AssertionError("unknown/duplicate case");
    int total=0;
    for(String group:groups) {
      int ran=0,sum=0;
      for(Case c:cases)if(c.group.equals(group)) {
        checks=0;failures=0;c.body.run();
        if(checks==0||failures!=0)throw new AssertionError("no successful assertions: "+c.name);
        System.out.println("CASE "+group+" "+c.name+" checks="+checks);sum+=checks;ran++;total++;
      }
      if(ran==0)throw new AssertionError("missing group: "+group);
      System.out.println("GROUP "+group+" cases="+ran+" checks="+sum+" COMPLETE");
    }
    if(total!=cases.size())throw new AssertionError("unrun cases");
    System.out.println("TARGET_COMPLETE cases="+total);
  }
}
