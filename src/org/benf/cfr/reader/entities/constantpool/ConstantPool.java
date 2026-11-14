package org.benf.cfr.reader.entities.constantpool;

import org.benf.cfr.reader.entities.ClassFile;
import org.benf.cfr.reader.state.ClassCache;
import org.benf.cfr.reader.state.DCCommonState;
import org.benf.cfr.reader.util.ConfusedCFRException;
import org.benf.cfr.reader.util.bytestream.ByteData;
import org.benf.cfr.reader.util.bytestream.OffsettingByteData;
import org.benf.cfr.reader.util.collections.ListFactory;
import org.benf.cfr.reader.util.collections.MapFactory;
import org.benf.cfr.reader.util.getopt.Options;
import org.benf.cfr.reader.util.output.LoggerFactory;

import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Logger;

public class ConstantPool {
    private static final Logger logger = LoggerFactory.create(ConstantPool.class);

    private final long length;
    private final List<ConstantPoolEntry> entries;
    private final Options options;
    private final DCCommonState dcCommonState;
    private final ClassCache classCache;
    private final ClassFile classFile;
    private String comparisonKey;
    private boolean isLoaded;
    private final int idx = sidx.getAndIncrement();
    private final static AtomicInteger sidx = new AtomicInteger();
    private final boolean dynamicConstants;
    private final List<Integer> dynamicConstantIndices;
    private Map<Integer, CondySymbol> condySymbols;

    public ConstantPool(ClassFile classFile, DCCommonState dcCommonState, ByteData raw, int count) {
        this.classFile = classFile;
        this.options = dcCommonState.getOptions();
        count--;

        RawTmp tmp = processRaw(raw, count);
        this.entries = tmp.entries;
        this.length = tmp.rawLength;
        this.dynamicConstants = tmp.dynamicConstants;
        this.dynamicConstantIndices = tmp.dynamicConstantIndices;
        this.dcCommonState = dcCommonState;
        this.classCache = dcCommonState.getClassCache();
        this.isLoaded = true;
    }

    public DCCommonState getDCCommonState() {
        return dcCommonState;
    }

    public boolean isLoaded() {
        return isLoaded;
    }

    public boolean isDynamicConstants() {
        return dynamicConstants;
    }

    /*
     * Raw constant pool indices of all CONSTANT_Dynamic entries, in pool order.
     */
    public List<Integer> getDynamicConstantIndices() {
        return dynamicConstantIndices;
    }

    public ClassFile getClassFile() {
        return classFile;
    }

    /*
     * Symbols are cached per constant pool, so every read of the same dynamic constant
     * (from any method, or as a repeated bootstrap argument) yields the same node.
     */
    public CondySymbol getCondySymbol(int index) {
        if (condySymbols == null) condySymbols = MapFactory.newMap();
        CondySymbol symbol = condySymbols.get(index);
        if (symbol == null) {
            symbol = new CondySymbol(this, index);
            condySymbols.put(index, symbol);
        }
        return symbol;
    }

    private static class RawTmp {
        final List<ConstantPoolEntry> entries;
        final long rawLength;
        final boolean dynamicConstants;
        final List<Integer> dynamicConstantIndices;

        RawTmp(List<ConstantPoolEntry> entries, long rawLength, boolean dynamicConstants, List<Integer> dynamicConstantIndices) {
            this.entries = entries;
            this.rawLength = rawLength;
            this.dynamicConstants = dynamicConstants;
            this.dynamicConstantIndices = dynamicConstantIndices;
        }
    }

    private RawTmp processRaw(ByteData raw, int count) {
        List<ConstantPoolEntry> tgt = ListFactory.newList(count);
        List<Integer> dynamicConstantIndices = ListFactory.newList();
        OffsettingByteData data = raw.getOffsettingOffsetData(0);
        boolean dynamicConstant = false;
        logger.info("Processing " + count + " constpool entries.");
        for (int x = 0; x < count; ++x) {
            ConstantPoolEntry.Type type = ConstantPoolEntry.Type.get(data.getS1At(0));
            ConstantPoolEntry cpe;
            switch (type) {
                case CPT_NameAndType:
                    cpe = new ConstantPoolEntryNameAndType(this, data);
                    break;
                case CPT_String:
                    cpe = new ConstantPoolEntryString(this, data);
                    break;
                case CPT_FieldRef:
                    cpe = new ConstantPoolEntryFieldRef(this, data);
                    break;
                case CPT_MethodRef:
                    cpe = new ConstantPoolEntryMethodRef(this, data, false);
                    break;
                case CPT_InterfaceMethodRef:
                    cpe = new ConstantPoolEntryMethodRef(this, data, true);
                    break;
                case CPT_Class:
                    cpe = new ConstantPoolEntryClass(this, data);
                    break;
                case CPT_Double:
                    cpe = new ConstantPoolEntryDouble(this, data);
                    break;
                case CPT_Float:
                    cpe = new ConstantPoolEntryFloat(this, data);
                    break;
                case CPT_Long:
                    cpe = new ConstantPoolEntryLong(this, data);
                    break;
                case CPT_Integer:
                    cpe = new ConstantPoolEntryInteger(this, data);
                    break;
                case CPT_UTF8:
                    cpe = new ConstantPoolEntryUTF8(this, data, options);
                    break;
                case CPT_MethodHandle:
                    cpe = new ConstantPoolEntryMethodHandle(this, data);
                    break;
                case CPT_MethodType:
                    cpe = new ConstantPoolEntryMethodType(this, data);
                    break;
                case CPT_DynamicInfo:
                    cpe = new ConstantPoolEntryDynamicInfo(this, data, x + 1);
                    dynamicConstant = true;
                    dynamicConstantIndices.add(x + 1);
                    break;
                case CPT_InvokeDynamic:
                    cpe = new ConstantPoolEntryInvokeDynamic(this, data);
                    break;
                case CPT_ModuleInfo:
                    cpe = new ConstantPoolEntryModuleInfo(this, data);
                    break;
                case CPT_PackageInfo:
                    cpe = new ConstantPoolEntryPackageInfo(this, data);
                    break;
                default:
                    throw new ConfusedCFRException("Invalid constant pool entry : " + type);
            }
            logger.info("" + (x + 1) + " : " + cpe);
            tgt.add(cpe);
            switch (type) {
                case CPT_Double:
                case CPT_Long:
                    tgt.add(null);
                    x++;
                    break;
            }

            long size = cpe.getRawByteLength();
            data.advance(size);
        }
        return new RawTmp(tgt, data.getOffset(), dynamicConstant, dynamicConstantIndices);
    }

    public long getRawByteLength() {
        return length;
    }

    public ConstantPoolEntry getEntry(int index) {
        if (index == 0) throw new ConfusedCFRException("Attempt to fetch element 0 from constant pool");
        // NB: Constant pool entries are 1 based.
        if (index > entries.size()) {
            throw new IndexOutOfBoundsException("Constant pool has " + entries.size() + " entries - attempted to access entry #" + (index-1));
        }
        return entries.get(index - 1);
    }

    public ConstantPoolEntryUTF8 getUTF8Entry(int index) {
        return (ConstantPoolEntryUTF8) getEntry(index);
    }

    public ConstantPoolEntryNameAndType getNameAndTypeEntry(int index) {
        return (ConstantPoolEntryNameAndType) getEntry(index);
    }

    public ConstantPoolEntryMethodHandle getMethodHandleEntry(int index) {
        return (ConstantPoolEntryMethodHandle) getEntry(index);
    }

    ConstantPoolEntryMethodRef getMethodRefEntry(int index) {
        ConstantPoolEntry entry = getEntry(index);
        return (ConstantPoolEntryMethodRef) entry;
    }

    ConstantPoolEntryFieldRef getFieldRefEntry(int index) {
        ConstantPoolEntry entry = getEntry(index);
        return (ConstantPoolEntryFieldRef) entry;
    }

    public ConstantPoolEntryClass getClassEntry(int index) {
        return (ConstantPoolEntryClass) getEntry(index);
    }

    public ConstantPoolEntryModuleInfo getModuleEntry(int index) {
        return (ConstantPoolEntryModuleInfo) getEntry(index);
    }

    public ConstantPoolEntryPackageInfo getPackageEntry(int index) {
        return (ConstantPoolEntryPackageInfo) getEntry(index);
    }

    public ClassCache getClassCache() {
        return classCache;
    }

    @Override
    public boolean equals(Object o) {
        getComparisonKey();
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;

        ConstantPool that = (ConstantPool) o;

        if (!comparisonKey.equals(that.comparisonKey)) return false;

        return true;
    }

    @Override
    public String toString() {
        return getComparisonKey() + "[" + idx + "]";
    }

    @Override
    public int hashCode() {
        return getComparisonKey().hashCode();
    }

    private String getComparisonKey() {
        if (comparisonKey == null) comparisonKey = classFile.getFilePath();
        return comparisonKey;
    }
}
