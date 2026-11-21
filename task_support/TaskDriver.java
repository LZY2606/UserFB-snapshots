package task_support;
public final class TaskDriver {
  public static void main(String[] args) throws Exception {
    TaskSuite suite=(TaskSuite)Class.forName("BlendWeightSolverTest").getDeclaredConstructor().newInstance();
    Checks checks=new Checks();suite.run(checks);checks.finish();
  }
}
