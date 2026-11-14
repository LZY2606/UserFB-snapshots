package task_support;
import java.nio.file.*;

public final class Driver {
    public static void main(String[] args) throws Exception {
        Baseline.main(new String[0]);
        Class.forName("TextSourceRegression").getMethod("run").invoke(null);
        Checks.finish();
        Files.write(Paths.get(args[0]), "TEXT_SOURCE_ALL_OK".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        System.out.println("TEXT_SOURCE_ALL_OK");
    }
}
