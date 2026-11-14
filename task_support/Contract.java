import java.nio.file.Path;
import java.nio.file.Paths;

/** Counts assertions in the added regression groups; no target implementation is supplied. */
public final class Contract {
    private static long assertions;
    public static void check(boolean condition, String detail) {
        assertions++;
        if (!condition) throw new AssertionError(detail);
    }
    private static void ran(long before, String group) {
        if (assertions == before) throw new AssertionError("zero assertions in " + group);
    }
    public static void main(String[] args) throws Exception {
        Path inputs = Paths.get(args[0]);
        long before = assertions;
        CondyContract.graph(inputs); ran(before, "graph");
        before = assertions;
        CondyContract.types(inputs); ran(before, "types");
        before = assertions;
        CondyContract.consumers(inputs); ran(before, "consumers");
        System.out.println("CFR_CONDY_CONTRACT_OK assertions=" + assertions);
    }
}
