package task_support;
import java.util.LinkedHashMap;
import java.util.Map;
public final class Checks {
  public interface Body { void run() throws Exception; }
  private final Map<String,Integer> groups=new LinkedHashMap<>();
  private String active;
  private int checks;
  public void check(boolean value,String message) {
    if(active==null)throw new AssertionError("check outside group");
    checks++;
    if(!value)throw new AssertionError(message);
  }
  public void group(String name,Body body) throws Exception {
    if(active!=null||groups.containsKey(name))throw new AssertionError("nested/duplicate group");
    active=name;checks=0;
    try {body.run();if(checks==0)throw new AssertionError("group has no checks: "+name);}
    finally {active=null;}
    groups.put(name,checks);
    System.out.println("GROUP "+name+" checks="+checks+" COMPLETE");
  }
  void finish() {
    String[] expected={"forward","jacobian","simplex","integration","invalid"};
    if(groups.size()!=expected.length)throw new AssertionError("wrong group count");
    for(String g:expected)if(!groups.containsKey(g)||groups.get(g)<=0)throw new AssertionError("missing group "+g);
    System.out.println("TARGET_COMPLETE groups="+groups.size());
  }
}
