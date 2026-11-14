/**
 * Small companion example for CONDY_SYMBOLS.md.
 *
 * javac does not emit CONSTANT_Dynamic entries for ordinary source, so this class simply
 * mirrors the shapes that the synthetic class file (see SyntheticCondy) exercises: methods
 * that look like bootstraps, a string concat, and a lambda.  Compile it and decompile it to
 * see that classes without condy keep CFR's usual output - no Condy.# symbols appear.
 *
 * The condy symbol output itself is produced from CONSTANT_Dynamic constant pool entries,
 * which other compilers (or bytecode generators) emit for constants such as:
 *
 *   long        number      // a J constant resolved by a custom bootstrap
 *   int[]       flags       // an array constant - keeps its array type
 *   ClassDesc   intClass    // reducible via ConstantBootstraps.primitiveClass -> int.class
 *
 * At decompile time those appear as typed symbolic references (Condy.#N) plus a class level
 * legend, instead of an inlined pseudo-call to the bootstrap method.
 */
public class CondyExample {
    // Shaped like a condy bootstrap method: lookup, name and type, then static arguments.
    public static Object leaf(java.lang.invoke.MethodHandles.Lookup lookup, String name, Class<?> type) {
        return 1L;
    }

    // A varargs bootstrap shape - static arguments keep their order in the legend.
    public static Object gather(java.lang.invoke.MethodHandles.Lookup lookup, String name, Class<?> type, Object... args) {
        return null;
    }

    // String concat: rewritten only when the recipe is genuinely known.
    public static String label(long value) {
        return "value=" + value;
    }

    // Plain lambda control: unaffected by condy handling.
    public static Runnable task() {
        return () -> { };
    }

    public static void main(String[] args) {
        System.out.println(label(7));
        task().run();
    }
}
