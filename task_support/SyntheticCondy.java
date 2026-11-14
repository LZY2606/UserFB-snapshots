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
    private int nt(String name, String desc) throws IOException { return add(12, utf(name), utf(desc)); }
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
    private void method(String name, String desc, int stack, int locals, int... code) throws IOException {
        int n = utf(name), d = utf(desc), codeName = utf("Code");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeShort(0x0009); out.writeShort(n); out.writeShort(d); out.writeShort(1);
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
        s.bootstraps.add(new int[]{leaf});
        int number = s.dynamic(0, "number", "J");
        s.bootstraps.add(new int[]{outer, number, number});
        int pair = s.dynamic(1, "pair", "J");
        int box = s.add(10, s.clazz("java/lang/Long"), s.nt("valueOf", "(J)Ljava/lang/Long;"));
        s.method("leaf", head + ")Ljava/lang/Object;", 2, 3, 10, 184, box >>> 8, box, 176);
        s.method("outer", head + "JJ)Ljava/lang/Object;", 4, 7, 33, 22, 5, 97, 184, box >>> 8, box, 176);
        s.method("ordinary", "()I", 1, 0, 16, 7, 172);
        s.method("number", "()J", 2, 0, 20, number >>> 8, number, 173);
        s.method("pair", "()J", 2, 0, 20, pair >>> 8, pair, 173);
        s.write(dir.resolve("CondySample.class"), owner, parent);
        System.out.println("SYNTHETIC_CONDY number=#" + number + " pair=#" + pair + " shared-arg-slots=2");
    }
}
