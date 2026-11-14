package org.benf.cfr.reader.entities.constantpool;

import org.benf.cfr.reader.bytecode.analysis.parse.literal.TypedLiteral;
import org.benf.cfr.reader.bytecode.analysis.types.JavaTypeInstance;
import org.benf.cfr.reader.entities.bootstrap.BootstrapMethodInfo;
import org.benf.cfr.reader.entities.bootstrap.MethodHandleBehaviour;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/*
 * A symbolic view of a CONSTANT_Dynamic (condy) constant pool entry.
 *
 * The identity of a symbol is the owning constant pool (i.e. the owning class) together with
 * the raw constant pool index.  Two entries with identical content but different indices are
 * different symbols, and every read of the same entry yields the same (cached) symbol node.
 *
 * The type of the symbol is taken from the field descriptor of the entry's NameAndType.  It is
 * deliberately NOT weakened to the declared return type of the bootstrap method - a condy whose
 * bootstrap declares Object but whose descriptor is J (or D, or an array type) keeps its real
 * type.
 *
 * Static bootstrap arguments form an ordered list of dependency edges; nested dynamic constants
 * are shared symbol nodes (via the constant pool cache), never copied into ever larger trees.
 *
 * We only read the constant pool structure - we never load the referenced classes, execute the
 * bootstrap method, or attempt to recover the runtime value of an arbitrary constant.
 */
public class CondySymbol {
    private final ConstantPool constantPool;
    private final int index;
    private final String name;
    private final String descriptor;
    private final JavaTypeInstance type;
    private final MethodHandleBehaviour bootstrapKind;
    private final String bootstrapOwner;
    private final String bootstrapName;
    private final String bootstrapDescriptor;
    /*
     * Ordered static bootstrap arguments.  Each entry is either a TypedLiteral or a CondySymbol.
     * A nested condy appearing in multiple argument positions is the same node in each position.
     */
    private final List<Object> staticArgs;

    public CondySymbol(ConstantPool constantPool, int index) {
        this.constantPool = constantPool;
        this.index = index;
        ConstantPoolEntryDynamicInfo entry = (ConstantPoolEntryDynamicInfo) constantPool.getEntry(index);
        ConstantPoolEntryNameAndType nameAndType = entry.getNameAndTypeEntry();
        this.name = nameAndType.getName().getValue();
        this.descriptor = nameAndType.getDescriptor().getValue();
        this.type = nameAndType.decodeTypeTok();
        BootstrapMethodInfo bootstrap = constantPool.getClassFile().getBootstrapMethods().getBootStrapMethodInfo(entry.getBootstrapMethodAttrIndex());
        this.bootstrapKind = bootstrap.getMethodHandleBehaviour();
        ConstantPoolEntryMethodRef methodRef = bootstrap.getConstantPoolEntryMethodRef();
        this.bootstrapOwner = methodRef.getClassEntry().getTypeInstance().toString();
        this.bootstrapName = methodRef.getName();
        this.bootstrapDescriptor = methodRef.getNameAndTypeEntry().getDescriptor().getValue();
        List<Object> args = new ArrayList<Object>();
        for (ConstantPoolEntry arg : bootstrap.getBootstrapArguments()) {
            if (arg instanceof ConstantPoolEntryDynamicInfo) {
                args.add(constantPool.getCondySymbol(((ConstantPoolEntryDynamicInfo) arg).getConstantPoolIndex()));
            } else {
                args.add(TypedLiteral.getConstantPoolEntry(constantPool, arg));
            }
        }
        this.staticArgs = Collections.unmodifiableList(args);
    }

    public ConstantPool getConstantPool() {
        return constantPool;
    }

    public int getIndex() {
        return index;
    }

    public String getName() {
        return name;
    }

    public String getDescriptor() {
        return descriptor;
    }

    public JavaTypeInstance getType() {
        return type;
    }

    public MethodHandleBehaviour getBootstrapKind() {
        return bootstrapKind;
    }

    public String getBootstrapOwner() {
        return bootstrapOwner;
    }

    public String getBootstrapName() {
        return bootstrapName;
    }

    public String getBootstrapDescriptor() {
        return bootstrapDescriptor;
    }

    public List<Object> getStaticArgs() {
        return staticArgs;
    }

    /*
     * How the symbol is referenced from a use site in decompiled output.  Clearly pseudocode -
     * it names the owning class level legend entry rather than any resolvable java expression.
     */
    public String getReference() {
        return "Condy.#" + index;
    }

    private String getShortReference() {
        return "#" + index;
    }

    /*
     * One line of the class level legend - name, descriptor, the bootstrap handle's kind, owner,
     * member name and full descriptor, and the ordered static arguments.  Nested condy arguments
     * refer back to their own legend entries by index.
     */
    public String describe() {
        StringBuilder sb = new StringBuilder();
        sb.append('#').append(index).append(" \"").append(name).append("\" : ").append(descriptor);
        sb.append(" <- ").append(bootstrapKind).append(' ').append(bootstrapOwner).append('.').append(bootstrapName);
        sb.append(' ').append(bootstrapDescriptor);
        sb.append(" args=[");
        for (int x = 0; x < staticArgs.size(); ++x) {
            if (x > 0) sb.append(", ");
            Object arg = staticArgs.get(x);
            sb.append(arg instanceof CondySymbol ? ((CondySymbol) arg).getShortReference() : arg.toString());
        }
        sb.append(']');
        return sb.toString();
    }

    /*
     * Class level legend, in raw constant pool index order.  Emitted as decompiler comments -
     * it describes the constants, it does not (and cannot) resolve them.
     */
    public static List<String> legend(ConstantPool constantPool) {
        List<String> lines = new ArrayList<String>();
        lines.add("Dynamic constant symbols (pseudocode - bootstrap methods are not executed):");
        for (int index : constantPool.getDynamicConstantIndices()) {
            lines.add(constantPool.getCondySymbol(index).describe());
        }
        return lines;
    }

    @Override
    public boolean equals(Object o) {
        if (o == this) return true;
        if (!(o instanceof CondySymbol)) return false;
        CondySymbol other = (CondySymbol) o;
        return constantPool == other.constantPool && index == other.index;
    }

    @Override
    public int hashCode() {
        return System.identityHashCode(constantPool) * 31 + index;
    }

    @Override
    public String toString() {
        return getReference();
    }
}
