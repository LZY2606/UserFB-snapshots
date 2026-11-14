import org.benf.cfr.reader.api.CfrDriver;
import org.benf.cfr.reader.api.OutputSinkFactory;
import org.benf.cfr.reader.bytecode.analysis.parse.utils.ComparableUnderEC;
import org.benf.cfr.reader.bytecode.analysis.parse.utils.EquivalenceConstraint;
import org.benf.cfr.reader.entities.ClassFile;
import org.benf.cfr.reader.state.ClassFileSourceImpl;
import org.benf.cfr.reader.state.DCCommonState;
import org.benf.cfr.reader.util.bytestream.BaseByteData;
import org.benf.cfr.reader.util.getopt.Options;
import org.benf.cfr.reader.util.getopt.OptionsImpl;

import java.io.ByteArrayOutputStream;
import java.io.PrintStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.List;

/**
 * Shared helpers for the condy contract regression - everything goes through CFR's production
 * entry points (Main / CfrDriver) or its production model classes (ClassFile, ConstantPool).
 */
final class CondyContractSupport {

    /** Decompile via the production Main entry point, capturing stdout. */
    static String decompileWithMain(Path classFile) throws Exception {
        PrintStream previous = System.out;
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        System.setOut(new PrintStream(buffer, true, "UTF-8"));
        try {
            org.benf.cfr.reader.Main.main(new String[]{classFile.toString(), "--silent", "true"});
        } finally {
            System.setOut(previous);
        }
        return new String(buffer.toByteArray(), "UTF-8");
    }

    /** Decompile via the production CfrDriver API, capturing the java sink. */
    static String decompileWithDriver(Path classFile) {
        final StringBuilder result = new StringBuilder();
        OutputSinkFactory sinkFactory = new OutputSinkFactory() {
            @Override
            public List<SinkClass> getSupportedSinks(SinkType sinkType, Collection<SinkClass> available) {
                return sinkType == SinkType.JAVA ? Arrays.asList(SinkClass.STRING) : Collections.<SinkClass>emptyList();
            }

            @Override
            @SuppressWarnings("unchecked")
            public <T> Sink<T> getSink(SinkType sinkType, SinkClass sinkClass) {
                if (sinkType != SinkType.JAVA) return null;
                return new Sink<T>() {
                    @Override
                    public void write(T sinkable) {
                        result.append(sinkable);
                    }
                };
            }
        };
        new CfrDriver.Builder().withOutputSink(sinkFactory).build().analyse(Arrays.asList(classFile.toString()));
        return result.toString();
    }

    /** Load a class file through CFR's own model (used to inspect production expressions). */
    static ClassFile loadClassFile(Path path) throws Exception {
        Options options = OptionsImpl.getFactory().create(Collections.<String, String>emptyMap());
        DCCommonState state = new DCCommonState(options, new ClassFileSourceImpl(options));
        return new ClassFile(new BaseByteData(Files.readAllBytes(path)), path.toString(), state);
    }

    /** Extract the source of one method from decompiled output (from declaration to closing brace). */
    static String methodBody(String out, String name) {
        int at = out.indexOf(" " + name + "(");
        if (at < 0) throw new AssertionError("method not found in output: " + name);
        int end = out.indexOf("\n    }", at);
        return end < 0 ? out.substring(at) : out.substring(at, end);
    }

    /** A plain equality based equivalence constraint for expression comparisons. */
    static final EquivalenceConstraint EQUALITY = new EquivalenceConstraint() {
        @Override
        public boolean equivalent(Object o1, Object o2) {
            return o1 == null ? o2 == null : o1.equals(o2);
        }

        @Override
        @SuppressWarnings("rawtypes")
        public boolean equivalent(Collection o1, Collection o2) {
            return o1 == null ? o2 == null : o1.equals(o2);
        }

        @Override
        public boolean equivalent(ComparableUnderEC o1, ComparableUnderEC o2) {
            return o1 == null ? o2 == null : o1.equals(o2);
        }
    };
}
