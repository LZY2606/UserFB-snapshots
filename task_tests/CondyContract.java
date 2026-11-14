import org.benf.cfr.reader.bytecode.analysis.parse.expression.Literal;
import org.benf.cfr.reader.bytecode.analysis.parse.literal.TypedLiteral;
import org.benf.cfr.reader.bytecode.analysis.types.RawJavaType;
import org.benf.cfr.reader.entities.ClassFile;
import org.benf.cfr.reader.entities.constantpool.CondySymbol;
import org.benf.cfr.reader.entities.constantpool.ConstantPool;
import org.benf.cfr.reader.entities.constantpool.ConstantPoolEntryDynamicInfo;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * Contract for the typed condy symbol representation: symbolic identity (owning class + raw
 * constant pool index), descriptor faithful types, and shared nested graph structure - checked
 * both on the production expressions directly and on the output of the production entry points
 * (Main and CfrDriver).
 */
public final class CondyContract {

    /** The dynamic constants of CondySample, discovered through CFR's production model. */
    private static final class Symbols {
        ConstantPool cp;
        int number;
        int alias;
        int pair;
        int pi;
        int flags;
        int recipe;
        int bag;
        int known;

        CondySymbol numberSymbol() { return cp.getCondySymbol(number); }
        CondySymbol aliasSymbol() { return cp.getCondySymbol(alias); }
        CondySymbol pairSymbol() { return cp.getCondySymbol(pair); }
        CondySymbol piSymbol() { return cp.getCondySymbol(pi); }
        CondySymbol flagsSymbol() { return cp.getCondySymbol(flags); }
        CondySymbol recipeSymbol() { return cp.getCondySymbol(recipe); }
        CondySymbol bagSymbol() { return cp.getCondySymbol(bag); }
    }

    private static Symbols symbols(Path inputs) throws Exception {
        ClassFile classFile = CondyContractSupport.loadClassFile(inputs.resolve("CondySample.class"));
        Symbols s = new Symbols();
        s.cp = classFile.getConstantPool();
        List<Integer> numbers = new ArrayList<Integer>();
        for (int index : s.cp.getDynamicConstantIndices()) {
            ConstantPoolEntryDynamicInfo entry = (ConstantPoolEntryDynamicInfo) s.cp.getEntry(index);
            String name = entry.getNameAndTypeEntry().getName().getValue();
            if (name.equals("number")) {
                numbers.add(index);
            } else if (name.equals("pair")) {
                s.pair = index;
            } else if (name.equals("pi")) {
                s.pi = index;
            } else if (name.equals("flags")) {
                s.flags = index;
            } else if (name.equals("recipe")) {
                s.recipe = index;
            } else if (name.equals("bag")) {
                s.bag = index;
            } else if (name.equals("I")) {
                s.known = index;
            }
        }
        // Two entries with identical name, descriptor and bootstrap but different indices.
        Contract.check(numbers.size() == 2, "expected two 'number' dynamic constants, got " + numbers);
        s.number = Math.min(numbers.get(0), numbers.get(1));
        s.alias = Math.max(numbers.get(0), numbers.get(1));
        Contract.check(s.pair > 0 && s.pi > 0 && s.flags > 0 && s.recipe > 0 && s.bag > 0 && s.known > 0,
                "missing dynamic constants in " + s.cp.getDynamicConstantIndices());
        return s;
    }

    private static String[] bothOutputs(Path inputs) throws Exception {
        Path classFile = inputs.resolve("CondySample.class");
        return new String[]{
                CondyContractSupport.decompileWithMain(classFile),
                CondyContractSupport.decompileWithDriver(classFile)
        };
    }

    /*
     * Graph structure: identity is owning class + raw index, shared nested nodes are shared
     * (never copied), ordered dependency edges keep every argument position.
     */
    public static void graph(Path inputs) throws Exception {
        Symbols s = symbols(inputs);

        // Same entry read repeatedly is the same node.
        Contract.check(s.cp.getCondySymbol(s.number) == s.numberSymbol(), "condy symbol is not a shared node");
        Contract.check(s.numberSymbol().equals(s.numberSymbol()), "symbol not equal to itself");

        // Identical content at a different index is a different symbol - never merged.
        Contract.check(s.number != s.alias, "test constants collapsed to one index");
        Contract.check(s.aliasSymbol() != s.numberSymbol(), "distinct indices share a node");
        Contract.check(!s.aliasSymbol().equals(s.numberSymbol()), "distinct indices compare equal");
        Contract.check(s.aliasSymbol().getName().equals(s.numberSymbol().getName()), "control: aliases differ in name");
        Contract.check(s.aliasSymbol().getDescriptor().equals(s.numberSymbol().getDescriptor()), "control: aliases differ in descriptor");
        Contract.check(s.aliasSymbol().getBootstrapName().equals(s.numberSymbol().getBootstrapName()), "control: aliases differ in bootstrap");

        // Nested graph: pair depends on number in two ordered argument positions, both present,
        // and both are the SAME shared node.
        List<Object> pairArgs = s.pairSymbol().getStaticArgs();
        Contract.check(pairArgs.size() == 2, "pair should have two ordered dependency edges, got " + pairArgs.size());
        Contract.check(pairArgs.get(0) == s.numberSymbol(), "pair arg 0 is not the shared number node");
        Contract.check(pairArgs.get(1) == s.numberSymbol(), "pair arg 1 is not the shared number node");
        Contract.check(s.pairSymbol().getStaticArgs() == s.pairSymbol().getStaticArgs(), "dependency edges not stable");

        // bag (varargs bootstrap) keeps its ordered edges too: nested condy then a string.
        List<Object> bagArgs = s.bagSymbol().getStaticArgs();
        Contract.check(bagArgs.size() == 2, "bag should have two ordered dependency edges");
        Contract.check(bagArgs.get(0) == s.numberSymbol(), "bag arg 0 is not the shared number node");
        Contract.check(bagArgs.get(1) instanceof TypedLiteral, "bag arg 1 should be a plain literal");
        Contract.check(((TypedLiteral) bagArgs.get(1)).getType() == TypedLiteral.LiteralType.String, "bag arg 1 should be a string");

        // Identity survives cloning and equivalence comparison of production expressions.
        Literal literal = new Literal(TypedLiteral.getCondy(s.numberSymbol()));
        Contract.check(literal.deepClone(null) == literal, "clone does not preserve the symbol node");
        Literal sameSymbol = new Literal(TypedLiteral.getCondy(s.numberSymbol()));
        Literal otherSymbol = new Literal(TypedLiteral.getCondy(s.aliasSymbol()));
        Contract.check(literal.equals(sameSymbol), "equal symbols compare unequal as expressions");
        Contract.check(!literal.equals(otherSymbol), "distinct symbols compare equal as expressions");
        Contract.check(literal.equivalentUnder(sameSymbol, CondyContractSupport.EQUALITY), "equal symbols not equivalent");
        Contract.check(!literal.equivalentUnder(otherSymbol, CondyContractSupport.EQUALITY), "distinct symbols equivalent");
        Contract.check(TypedLiteral.getCondy(s.numberSymbol()).equals(TypedLiteral.getCondy(s.numberSymbol())), "equal symbol literals unequal");
        Contract.check(!TypedLiteral.getCondy(s.numberSymbol()).equals(TypedLiteral.getCondy(s.aliasSymbol())), "distinct symbol literals equal");

        // Output: the legend lists every symbol in raw constant pool index order, and every
        // legend line is the describe() text of the production symbol.
        for (String out : bothOutputs(inputs)) {
            Contract.check(out.contains("Dynamic constant symbols"), "no symbol legend in output");
            int previous = -1;
            for (int index : s.cp.getDynamicConstantIndices()) {
                String line = s.cp.getCondySymbol(index).describe();
                Contract.check(out.contains(line), "legend line missing from output: " + line);
                int at = out.indexOf("#" + index + " \"");
                Contract.check(at > previous, "legend not in raw index order at #" + index);
                previous = at;
            }
            // Both argument positions of the shared child appear in the legend, by reference.
            Contract.check(out.contains("args=[#" + s.number + ", #" + s.number + "]"),
                    "shared child not referenced from both argument positions");
            // Use sites reference the legend: the two aliases are distinct references.
            Contract.check(CondyContractSupport.methodBody(out, "number").contains("return Condy.#" + s.number + ";"),
                    "number() does not reference its symbol");
            Contract.check(CondyContractSupport.methodBody(out, "numberAlias").contains("return Condy.#" + s.alias + ";"),
                    "numberAlias() does not reference its own symbol");
            Contract.check(CondyContractSupport.methodBody(out, "pair").contains("return Condy.#" + s.pair + ";"),
                    "pair() does not reference its symbol");
        }
    }

    /*
     * Types come from the NameAndType field descriptor - J, D and array types must not be
     * weakened to Object or String even when the bootstrap declares an Object return.
     */
    public static void types(Path inputs) throws Exception {
        Symbols s = symbols(inputs);

        Contract.check(RawJavaType.LONG.equals(s.numberSymbol().getType()), "J descriptor not typed long");
        Contract.check(RawJavaType.DOUBLE.equals(s.piSymbol().getType()), "D descriptor not typed double");
        Contract.check(s.flagsSymbol().getType().getNumArrayDimensions() == 1, "array descriptor lost its dimensions");
        Contract.check(RawJavaType.INT.equals(s.flagsSymbol().getType().getArrayStrippedType()), "array descriptor not int[]");
        Contract.check("java.lang.String".equals(s.recipeSymbol().getType().getRawName()), "String descriptor not typed String");

        // bag's bootstrap declares Object - the constant is still J.
        Contract.check(s.bagSymbol().getBootstrapDescriptor().endsWith(")Ljava/lang/Object;"), "control: bag bootstrap should declare Object");
        Contract.check(RawJavaType.LONG.equals(s.bagSymbol().getType()), "J constant weakened to bootstrap return type");

        // The type lives on the production literal expression.
        Contract.check(RawJavaType.LONG.equals(TypedLiteral.getCondy(s.bagSymbol()).getInferredJavaType().getJavaTypeInstance()),
                "literal for J constant not typed long");
        Contract.check(RawJavaType.DOUBLE.equals(TypedLiteral.getCondy(s.piSymbol()).getInferredJavaType().getJavaTypeInstance()),
                "literal for D constant not typed double");
        Contract.check(TypedLiteral.getCondy(s.flagsSymbol()).getInferredJavaType().getJavaTypeInstance().getNumArrayDimensions() == 1,
                "literal for array constant not typed as array");
        Literal bagLiteral = new Literal(TypedLiteral.getCondy(s.bagSymbol()));
        Contract.check(RawJavaType.LONG.equals(bagLiteral.getInferredJavaType().getJavaTypeInstance()),
                "expression for J constant not typed long");

        // Output: typed use sites, no casts laundering the constants through Object or String.
        for (String out : bothOutputs(inputs)) {
            Contract.check(out.contains("public static double pi()"), "pi() lost its double return type");
            Contract.check(CondyContractSupport.methodBody(out, "pi").contains("return Condy.#" + s.pi + ";"),
                    "pi() does not return its symbol directly");
            Contract.check(out.contains("public static int[] flags()"), "flags() lost its int[] return type");
            Contract.check(CondyContractSupport.methodBody(out, "flags").contains("return Condy.#" + s.flags + ";"),
                    "flags() does not return its symbol directly");
            Contract.check(out.contains("public static long bag()"), "bag() lost its long return type");
            Contract.check(CondyContractSupport.methodBody(out, "bag").contains("return Condy.#" + s.bag + ";"),
                    "bag() does not return its symbol directly");
            Contract.check(!CondyContractSupport.methodBody(out, "bag").contains("(Object)"),
                    "bag() constant laundered through Object");
            Contract.check(!CondyContractSupport.methodBody(out, "bag").contains("String"),
                    "bag() constant laundered through String");
            // The legend records the descriptor faithful types.
            Contract.check(out.contains("#" + s.pi + " \"pi\" : D"), "legend lost the D descriptor");
            Contract.check(out.contains("#" + s.flags + " \"flags\" : [I"), "legend lost the [I descriptor");
            Contract.check(out.contains("#" + s.bag + " \"bag\" : J"), "legend lost the J descriptor");
        }
    }

    /*
     * Consumers: ldc, ldc_w, ldc2_w and invokedynamic bootstrap arguments all use the same
     * symbolic representation; known rewrites only proceed on genuinely known arguments.
     */
    public static void consumers(Path inputs) throws Exception {
        Symbols s = symbols(inputs);

        // The bootstrap handle details are part of the production symbol.
        Contract.check(s.recipeSymbol().getBootstrapKind().name().equals("INVOKE_STATIC"), "bootstrap kind not preserved");
        Contract.check("CondySample".equals(s.recipeSymbol().getBootstrapOwner()), "bootstrap owner not preserved");
        Contract.check("leaf".equals(s.recipeSymbol().getBootstrapName()), "bootstrap member name not preserved");
        Contract.check(s.recipeSymbol().getBootstrapDescriptor().startsWith("(Ljava/lang/invoke/MethodHandles$Lookup;"),
                "bootstrap descriptor not preserved");

        for (String out : bothOutputs(inputs)) {
            // ldc (recipe, index < 256), ldc_w (flags) and ldc2_w (number, pi, bag) all show
            // the same symbolic reference, cross-referencable with the legend.
            Contract.check(CondyContractSupport.methodBody(out, "recipe").contains("return Condy.#" + s.recipe + ";"),
                    "ldc consumer does not reference its symbol");
            Contract.check(CondyContractSupport.methodBody(out, "flags").contains("return Condy.#" + s.flags + ";"),
                    "ldc_w consumer does not reference its symbol");
            Contract.check(CondyContractSupport.methodBody(out, "number").contains("return Condy.#" + s.number + ";"),
                    "ldc2_w consumer does not reference its symbol");
            for (int index : s.cp.getDynamicConstantIndices()) {
                if (index == s.known) continue; // statically reduced - no symbolic use site.
                Contract.check(out.contains("Condy.#" + index), "no use site reference for #" + index);
                Contract.check(out.contains("#" + index + " \""), "no legend entry for use site #" + index);
            }

            // Known bootstrap static reduction keeps working: the primitiveClass condy is
            // reduced to a class literal, not left as a symbol.
            String knownClass = CondyContractSupport.methodBody(out, "knownClass");
            Contract.check(knownClass.contains("int.class"), "known bootstrap not statically reduced");
            Contract.check(!knownClass.contains("Condy.#"), "reducible constant left as a symbol");

            // StringConcatFactory with an unknown (dynamic constant) recipe: the invocation is
            // preserved with its original argument order, and the symbol's print text is NOT
            // treated as string content.
            String label = CondyContractSupport.methodBody(out, "label");
            Contract.check(label.contains("makeConcatWithConstants("), "unknown recipe: invocation not preserved");
            Contract.check(label.contains("Condy.#" + s.recipe), "unknown recipe: recipe symbol not an argument");
            Contract.check(label.indexOf("Condy.#" + s.recipe) < label.indexOf("(long)l"),
                    "unknown recipe: argument order not preserved");
            Contract.check(!label.contains("+"), "unknown recipe treated as string concat content");

            // Plain lambda control: unchanged output, no symbols involved.
            String task = CondyContractSupport.methodBody(out, "task");
            Contract.check(task.contains("::"), "lambda control not sugared as before");
            Contract.check(!task.contains("Condy.#"), "lambda control gained a spurious symbol");

            // Ordinary constants are untouched.
            Contract.check(out.contains("return 7;"), "ordinary constant output changed");
        }
    }
}
