package task_support;
public final class Checks {
 public interface CaseBody { void run() throws Exception; }
 private int checks, cases;
 public void check(boolean condition, String message) {
  ++checks;
  if(!condition) throw new AssertionError(message);
 }
 public void runCase(String name, CaseBody body) throws Exception {
  int before=checks;
  body.run();
  if(checks==before) throw new AssertionError("no checks in "+name);
  ++cases;
 }
 public int cases() { return cases; }
 public int checks() { return checks; }
}
