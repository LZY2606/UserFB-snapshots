package task_support;
public final class TargetDriver {
 public static void main(String[] args) throws Exception {
  Class<?> tests=Class.forName("task_tests.PGSCompositionTest");
  int total=0;
  for(String group: new String[]{"objects","snapshots","roundtrip"}) {
   Checks c=new Checks();
   tests.getMethod(group,Checks.class).invoke(null,c);
   if(c.cases()==0 || c.checks()==0) throw new AssertionError("empty group "+group);
   System.out.println("GROUP "+group+" cases="+c.cases()+" checks="+c.checks()+" COMPLETE");
   total+=c.cases();
  }
  System.out.println("TARGET_COMPLETE cases="+total);
 }
}
