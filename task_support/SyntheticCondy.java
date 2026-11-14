import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Writes benign class-file data only. Neither defines classes nor invokes bootstraps. */
public final class SyntheticCondy {
    private final List<byte[]> entries = new ArrayList<byte[]>();
    private final List<int[]> bootstraps = new ArrayList<int[]>();
    private final List<byte[]> methods = new ArrayList<byte[]>();
    private int add(int tag, int... words) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeByte(tag);
        for (int word : words) out.writeShort(word);
        entries.add(bytes.toByteArray());
        return entries.size();
    }
    private int utf(String text) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeByte(1); out.writeUTF(text); entries.add(bytes.toByteArray());
        return entries.size();
    }
    private int clazz(String text) throws IOException { return add(7, utf(text)); }
    private int string(String text) throws IOException { return add(8, utf(text)); }
    private int nt(String name, String desc) throws IOException { return add(12, utf(name), utf(desc)); }
    private int methodType(String desc) throws IOException { return add(16, utf(desc)); }
    private int handle(int owner, String name, String desc) throws IOException {
        int ref = add(10, owner, nt(name, desc));
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeByte(15); out.writeByte(6); out.writeShort(ref);
        entries.add(bytes.toByteArray()); return entries.size();
    }
    private int dynamic(int bootstrap, String name, String desc) throws IOException {
        return add(17, bootstrap, nt(name, desc));
    }
    private int indy(int bootstrap, String name, String desc) throws IOException {
        return add(18, bootstrap, nt(name, desc));
    }
    private void method(String name, String desc, int stack, int locals, int... code) throws IOException {
        method(0x0009, name, desc, stack, locals, code);
    }
    private void method(int flags, String name, String desc, int stack, int locals, int... code) throws IOException {
        int n = utf(name), d = utf(desc), codeName = utf("Code");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeShort(flags); out.writeShort(n); out.writeShort(d); out.writeShort(1);
        out.writeShort(codeName); out.writeInt(12 + code.length);
        out.writeShort(stack); out.writeShort(locals); out.writeInt(code.length);
        for (int op : code) out.writeByte(op);
        out.writeShort(0); out.writeShort(0); methods.add(bytes.toByteArray());
    }
    private void write(Path path, int owner, int parent) throws IOException {
        int attr = utf("BootstrapMethods");
        ByteArrayOutputStream bb = new ByteArrayOutputStream();
        DataOutputStream b = new DataOutputStream(bb);
        b.writeShort(bootstraps.size());
        for (int[] row : bootstraps) {
            b.writeShort(row[0]); b.writeShort(row.length - 1);
            for (int i = 1; i < row.length; i++) b.writeShort(row[i]);
        }
        try (DataOutputStream out = new DataOutputStream(Files.newOutputStream(path))) {
            out.writeInt(0xcafebabe); out.writeShort(0); out.writeShort(55);
            out.writeShort(entries.size() + 1);
            for (byte[] entry : entries) out.write(entry);
            out.writeShort(0x0021); out.writeShort(owner); out.writeShort(parent);
            out.writeShort(0); out.writeShort(0); out.writeShort(methods.size());
            for (byte[] method : methods) out.write(method);
            out.writeShort(1); out.writeShort(attr); out.writeInt(bb.size()); out.write(bb.toByteArray());
        }
    }
    public static void main(String[] args) throws Exception {
        Path dir = Paths.get(args[0]); Files.createDirectories(dir);
        SyntheticCondy s = new SyntheticCondy();
        int owner = s.clazz("CondySample"), parent = s.clazz("java/lang/Object");
        String head = "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/Class;";
        int leaf = s.handle(owner, "leaf", head + ")Ljava/lang/Object;");
        int outer = s.handle(owner, "outer", head + "JJ)Ljava/lang/Object;");
        int gather = s.handle(owner, "gather", head + "[Ljava/lang/Object;)Ljava/lang/Object;");
        int concatFactory = s.handle(s.clazz("java/lang/invoke/StringConcatFactory"), "makeConcatWithConstants",
                "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;");
        int lambdaMeta = s.handle(s.clazz("java/lang/invoke/LambdaMetafactory"), "metafactory",
                "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;");
        // b0 - leaf, no static args.
        s.bootstraps.add(new int[]{leaf});
        int number = s.dynamic(0, "number", "J");
        // Identical name, descriptor and bootstrap to number, but a different index - a distinct symbol.
        int numberAlias = s.dynamic(0, "number", "J");
        // b1 - outer, fixed args, shares number in two argument positions.
        s.bootstraps.add(new int[]{outer, number, number});
        int pair = s.dynamic(1, "pair", "J");
        int pi = s.dynamic(0, "pi", "D");
        int flags = s.dynamic(0, "flags", "[I");
        int recipe = s.dynamic(0, "recipe", "Ljava/lang/String;");
        // b2 - gather, varargs bootstrap with a nested condy and a string argument.
        int text = s.string("text");
        s.bootstraps.add(new int[]{gather, number, text});
        int bag = s.dynamic(2, "bag", "J");
        // b3 - string concat with an unknown (dynamic constant) recipe.
        s.bootstraps.add(new int[]{concatFactory, recipe});
        int concat = s.indy(3, "makeConcatWithConstants", "(J)Ljava/lang/String;");
        // b4 - plain lambda control.
        int samType = s.methodType("()V");
        int lambdaImpl = s.handle(owner, "lambda$task$0", "()V");
        s.bootstraps.add(new int[]{lambdaMeta, samType, lambdaImpl, samType});
        int run = s.indy(4, "run", "()Ljava/lang/Runnable;");
        // b5 - known bootstrap (ConstantBootstraps.primitiveClass): statically reducible.
        int constantBootstraps = s.handle(s.clazz("java/lang/invoke/ConstantBootstraps"), "primitiveClass",
                head + ")Ljava/lang/Class;");
        s.bootstraps.add(new int[]{constantBootstraps});
        int known = s.dynamic(5, "I", "Ljava/lang/Class;");
        int box = s.add(10, s.clazz("java/lang/Long"), s.nt("valueOf", "(J)Ljava/lang/Long;"));
        s.method("leaf", head + ")Ljava/lang/Object;", 2, 3, 10, 184, box >>> 8, box, 176);
        s.method("outer", head + "JJ)Ljava/lang/Object;", 4, 7, 33, 22, 5, 97, 184, box >>> 8, box, 176);
        s.method("gather", head + "[Ljava/lang/Object;)Ljava/lang/Object;", 1, 4, 1, 176);
        s.method("ordinary", "()I", 1, 0, 16, 7, 172);
        s.method("number", "()J", 2, 0, 20, number >>> 8, number, 173);
        s.method("numberAlias", "()J", 2, 0, 20, numberAlias >>> 8, numberAlias, 173);
        s.method("pair", "()J", 2, 0, 20, pair >>> 8, pair, 173);
        s.method("pi", "()D", 2, 0, 20, pi >>> 8, pi, 175);
        s.method("flags", "()[I", 1, 0, 19, flags >>> 8, flags, 176);
        s.method("recipe", "()Ljava/lang/String;", 1, 0, 18, recipe, 176);
        s.method("bag", "()J", 2, 0, 20, bag >>> 8, bag, 173);
        s.method("label", "(J)Ljava/lang/String;", 2, 2, 30, 186, concat >>> 8, concat, 0, 0, 176);
        s.method("task", "()Ljava/lang/Runnable;", 1, 0, 186, run >>> 8, run, 0, 0, 176);
        s.method("knownClass", "()Ljava/lang/Class;", 1, 0, 18, known, 176);
        s.method(0x000A, "lambda$task$0", "()V", 0, 0, 177);
        s.write(dir.resolve("CondySample.class"), owner, parent);
        System.out.println("SYNTHETIC_CONDY number=#" + number + " alias=#" + numberAlias + " pair=#" + pair
                + " pi=#" + pi + " flags=#" + flags + " recipe=#" + recipe + " bag=#" + bag
                + " known=#" + known + " shared-arg-slots=2");
    }
}
