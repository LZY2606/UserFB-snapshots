package com.linkedin.dex

import com.linkedin.dex.spec.ACC_PUBLIC
import java.io.ByteArrayOutputStream

/**
 * The dex header is always 0x70 bytes; the fields used by the parser live at fixed offsets within it.
 * See https://source.android.com/devices/tech/dalvik/dex-format#header-item
 */
const val DEX_HEADER_SIZE = 0x70
const val HEADER_FILE_SIZE_OFF = 0x20
const val HEADER_MAGIC_OFF = 0x00
const val HEADER_STRING_IDS_OFF = 0x3C
const val HEADER_TYPE_IDS_OFF = 0x44

/**
 * A minimal, in-memory dex file plus the offsets of every section inside it, so that tests can
 * corrupt individual bytes deterministically. The default fixture contains a single concrete class
 * "com.example.FooTest" with one virtual method per entry in `methodNames`, each annotated with
 * `@org.junit.Test`, so it is discoverable through the normal [com.linkedin.dex.parser.DexParser]
 * entry points.
 */
class DexFixture(
        val bytes: ByteArray,
        val stringIdsSize: Int,
        val stringIdsOff: Int,
        val typeIdsSize: Int,
        val typeIdsOff: Int,
        val methodIdsSize: Int,
        val methodIdsOff: Int,
        val classDefsOff: Int,
        val dataOff: Int,
        val stringDataOffs: List<Int>,
        val classDataOff: Int,
        val annotationsDirectoryOff: Int,
        val annotationSetOffs: List<Int>,
        val annotationItemOffs: List<Int>
)

/**
 * Builds a [DexFixture]. The corrupt variants are produced by overriding the pieces below:
 *
 * @param superclassIdx value written to the class_def's superclass_idx (use -1 for "no superclass")
 * @param methodIdxDiffs raw method_idx_diff values for the class_data virtual methods
 * @param classDataBytes full override of the class_data_item payload
 * @param directoryMethodIdxs method_idx values declared by the annotations directory
 * @param shareAnnotationSet if true, every directory entry points at the same annotation set
 * @param annotationElementValue if set, the annotation gets a single element named "value" whose
 *                               encoded_value bytes are these raw bytes
 */
fun buildDexFixture(
        methodNames: List<String> = listOf("testFoo"),
        superclassIdx: Int = 2,
        methodIdxDiffs: List<Int>? = null,
        classDataBytes: ByteArray? = null,
        directoryMethodIdxs: List<Int>? = null,
        shareAnnotationSet: Boolean = false,
        annotationElementValue: ByteArray? = null
): DexFixture {
    val strings = mutableListOf(
            "Lcom/example/FooTest;",
            "Lorg/junit/Test;",
            "Ljava/lang/Object;",
            "V")
    strings.addAll(methodNames)
    val valueNameIdx = if (annotationElementValue != null) {
        strings.add("value")
        strings.size - 1
    } else {
        -1
    }

    val typeCount = 4 // FooTest, org.junit.Test, Object, V
    val methodCount = methodNames.size

    val stringIdsOff = DEX_HEADER_SIZE
    val typeIdsOff = stringIdsOff + strings.size * 4
    val protoIdsOff = typeIdsOff + typeCount * 4
    val methodIdsOff = protoIdsOff + 12
    val classDefsOff = methodIdsOff + methodCount * 8
    val dataOff = classDefsOff + 32

    val diffs = methodIdxDiffs ?: methodNames.indices.map { index -> if (index == 0) 0 else 1 }
    val classData = classDataBytes ?: buildClassData(diffs)
    val directoryIdxs = directoryMethodIdxs ?: methodNames.indices.toList()
    val setCount = if (shareAnnotationSet) 1 else directoryIdxs.size

    val annotationItem = buildAnnotationItem(valueNameIdx, annotationElementValue)

    // data section layout: string data, annotations directory, annotation sets, annotation items, class data
    val data = ByteArrayOutputStream()
    val stringDataOffs = strings.map { value ->
        val offset = dataOff + data.size()
        data.write(uleb128(value.length))
        data.write(value.toByteArray(Charsets.UTF_8))
        data.write(0)
        offset
    }

    val annotationsDirectoryOff = dataOff + data.size()
    val annotationSetOffs = (0 until setCount).map { annotationsDirectoryOff + 16 + directoryIdxs.size * 8 + it * 8 }
    val annotationItemOffs = (0 until setCount).map {
        annotationSetOffs.first() + setCount * 8 + it * annotationItem.size
    }
    val classDataOff = annotationItemOffs.first() + setCount * annotationItem.size

    // annotations_directory_item
    data.writeIntLe(0) // classAnnotationsOff
    data.writeIntLe(0) // fieldsSize
    data.writeIntLe(directoryIdxs.size) // annotatedMethodsSize
    data.writeIntLe(0) // annotatedParametersSize
    directoryIdxs.forEachIndexed { index, methodIdx ->
        data.writeIntLe(methodIdx)
        data.writeIntLe(annotationSetOffs[if (shareAnnotationSet) 0 else index])
    }
    // annotation_set_items, each with a single entry
    annotationSetOffs.forEachIndexed { index, _ ->
        data.writeIntLe(1)
        data.writeIntLe(annotationItemOffs[index])
    }
    // annotation_items
    repeat(setCount) { data.write(annotationItem) }
    // class_data_item
    data.write(classData)

    val dataBytes = data.toByteArray()
    val bytes = ByteArray(dataOff + dataBytes.size)
    val buffer = java.nio.ByteBuffer.wrap(bytes).order(java.nio.ByteOrder.LITTLE_ENDIAN)

    // header
    buffer.put(byteArrayOf(0x64, 0x65, 0x78, 0x0A, 0x30, 0x33, 0x35, 0x00)) // magic: "dex\n035\0"
    buffer.position(HEADER_FILE_SIZE_OFF)
    buffer.putInt(bytes.size) // fileSize
    buffer.putInt(DEX_HEADER_SIZE) // headerSize
    buffer.putInt(0x12345678) // endianTag
    buffer.putInt(0) // linkSize
    buffer.putInt(0) // linkOff
    buffer.putInt(0) // mapOff
    buffer.putInt(strings.size)
    buffer.putInt(stringIdsOff)
    buffer.putInt(typeCount)
    buffer.putInt(typeIdsOff)
    buffer.putInt(1) // protoIdsSize
    buffer.putInt(protoIdsOff)
    buffer.putInt(0) // fieldIdsSize
    buffer.putInt(0) // fieldIdsOff
    buffer.putInt(methodCount)
    buffer.putInt(methodIdsOff)
    buffer.putInt(1) // classDefsSize
    buffer.putInt(classDefsOff)
    buffer.putInt(dataBytes.size) // dataSize
    buffer.putInt(dataOff)

    // string_ids
    stringDataOffs.forEach { buffer.putInt(it) }
    // type_ids: FooTest, org.junit.Test, Object, V
    buffer.putInt(0)
    buffer.putInt(1)
    buffer.putInt(2)
    buffer.putInt(3)
    // proto_ids: shorty "V", return type V, no parameters
    buffer.putInt(3)
    buffer.putInt(3)
    buffer.putInt(0)
    // method_ids
    for (index in 0 until methodCount) {
        buffer.putShort(0) // classIdx
        buffer.putShort(0) // protoIdx
        buffer.putInt(4 + index) // nameIdx
    }
    // class_def
    buffer.putInt(0) // classIdx
    buffer.putInt(ACC_PUBLIC) // accessFlags
    buffer.putInt(superclassIdx)
    buffer.putInt(0) // interfacesOff
    buffer.putInt(-1) // sourceFileIdx = NO_INDEX
    buffer.putInt(annotationsDirectoryOff)
    buffer.putInt(classDataOff)
    buffer.putInt(0) // staticValuesOff

    buffer.position(dataOff)
    buffer.put(dataBytes)

    return DexFixture(
            bytes = bytes,
            stringIdsSize = strings.size,
            stringIdsOff = stringIdsOff,
            typeIdsSize = typeCount,
            typeIdsOff = typeIdsOff,
            methodIdsSize = methodCount,
            methodIdsOff = methodIdsOff,
            classDefsOff = classDefsOff,
            dataOff = dataOff,
            stringDataOffs = stringDataOffs,
            classDataOff = classDataOff,
            annotationsDirectoryOff = annotationsDirectoryOff,
            annotationSetOffs = annotationSetOffs,
            annotationItemOffs = annotationItemOffs)
}

private fun buildClassData(methodIdxDiffs: List<Int>): ByteArray {
    val out = ByteArrayOutputStream()
    out.write(uleb128(0)) // staticFieldsSize
    out.write(uleb128(0)) // instanceFieldsSize
    out.write(uleb128(0)) // directMethodsSize
    out.write(uleb128(methodIdxDiffs.size)) // virtualMethodsSize
    methodIdxDiffs.forEach { diff ->
        out.write(uleb128(diff))
        out.write(uleb128(ACC_PUBLIC))
        out.write(uleb128(0)) // codeOff
    }
    return out.toByteArray()
}

private fun buildAnnotationItem(valueNameIdx: Int, elementValue: ByteArray?): ByteArray {
    val out = ByteArrayOutputStream()
    out.write(1) // visibility: runtime
    out.write(uleb128(1)) // typeIdx: Lorg/junit/Test;
    if (elementValue == null) {
        out.write(uleb128(0)) // no elements
    } else {
        out.write(uleb128(1)) // one element
        out.write(uleb128(valueNameIdx))
        out.write(elementValue)
    }
    return out.toByteArray()
}

fun uleb128(value: Int): ByteArray {
    var remaining = value
    val out = ArrayList<Byte>()
    do {
        var byte = remaining and 0x7F
        remaining = remaining ushr 7
        if (remaining != 0) {
            byte = byte or 0x80
        }
        out.add(byte.toByte())
    } while (remaining != 0)
    return out.toByteArray()
}

fun ByteArray.setIntLe(offset: Int, value: Int) {
    this[offset] = (value and 0xFF).toByte()
    this[offset + 1] = ((value ushr 8) and 0xFF).toByte()
    this[offset + 2] = ((value ushr 16) and 0xFF).toByte()
    this[offset + 3] = ((value ushr 24) and 0xFF).toByte()
}

private fun ByteArrayOutputStream.writeIntLe(value: Int) {
    write(value and 0xFF)
    write((value ushr 8) and 0xFF)
    write((value ushr 16) and 0xFF)
    write((value ushr 24) and 0xFF)
}
