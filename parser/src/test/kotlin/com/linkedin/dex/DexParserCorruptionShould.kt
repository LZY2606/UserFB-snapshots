package com.linkedin.dex

import com.linkedin.dex.parser.DexParser
import com.linkedin.dex.spec.DexException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Failure-path and bounds tests for the dex parser. Every test uses a small in-memory fixture
 * (see [buildDexFixture]) and goes through the normal [DexParser] entry points; corrupt inputs
 * must fail with a [DexException] at a stable, deterministic position instead of reading out of
 * bounds, looping, or throwing an unchecked exception.
 */
class DexParserCorruptionShould {

    @get:Rule
    val tempFolder = TemporaryFolder()

    // region minimal valid inputs (regression for the normal discovery path and its order)

    @Test
    fun layoutMinimalDexSectionsDeterministically() {
        val fixture = buildDexFixture()

        // the intermediate layout decisions of the minimal fixture are part of the regression:
        // all corruption tests below derive their expected failure positions from them
        assertEquals(DEX_HEADER_SIZE, fixture.stringIdsOff)
        assertEquals(fixture.stringIdsOff + fixture.stringIdsSize * 4, fixture.typeIdsOff)
        assertEquals(fixture.typeIdsOff + fixture.typeIdsSize * 4, fixture.methodIdsOff - 12)
        assertEquals(5, fixture.stringIdsSize)
        assertEquals(1, fixture.annotationSetOffs.size)
        assertEquals(1, fixture.annotationItemOffs.size)
        // string data (73 bytes) + directory (24) + one set (8) + one item (3) + class data (7)
        assertEquals(315, fixture.bytes.size)
        assertEquals(fixture.bytes.size - 7, fixture.classDataOff)
    }

    @Test
    fun findTestMethodInMinimalDex() {
        val fixture = buildDexFixture()

        val testNames = DexParser.findTestNames(writeApk(fixture.bytes))

        assertEquals(listOf("com.example.FooTest#testFoo"), testNames)
    }

    @Test
    fun keepDiscoveryOrderWhenMethodsShareOneAnnotationSet() {
        // two methods whose directory entries point at the same annotation set
        val fixture = buildDexFixture(
                methodNames = listOf("testFoo", "testBar"),
                shareAnnotationSet = true)

        val testMethods = DexParser.findTestMethods(writeApk(fixture.bytes))

        // discovery order stays sorted and both methods resolve the shared annotation
        assertEquals(listOf("com.example.FooTest#testBar", "com.example.FooTest#testFoo"),
                testMethods.map { it.testName })
        testMethods.forEach { testMethod ->
            assertTrue(testMethod.annotations.any { it.name == "org.junit.Test" })
        }
    }

    @Test
    fun findTestMethodWhenSuperclassIsMissing() {
        // superclass_idx = NO_INDEX, as it is for java.lang.Object itself
        val fixture = buildDexFixture(superclassIdx = -1)

        val testNames = DexParser.findTestNames(writeApk(fixture.bytes))

        assertEquals(listOf("com.example.FooTest#testFoo"), testNames)
    }

    // endregion

    // region header failure paths

    @Test
    fun rejectEmptyDexFile() {
        val exception = expectDexException(ByteArray(0))

        assertEquals("dex file too small: 0 bytes (header requires 112 bytes)", exception.message)
    }

    @Test
    fun rejectTruncatedHeader() {
        val fixture = buildDexFixture()
        val truncated = fixture.bytes.copyOf(0x40)

        val exception = expectDexException(truncated)

        assertEquals("dex file too small: 64 bytes (header requires 112 bytes)", exception.message)
    }

    @Test
    fun rejectInvalidMagic() {
        val bytes = buildDexFixture().bytes
        bytes[HEADER_MAGIC_OFF] = 0x00

        val exception = expectDexException(bytes)

        assertTrue(exception.message, exception.message!!.startsWith("Invalid dexMagic"))
    }

    // endregion

    // region string/type/method table failure paths

    @Test
    fun rejectStringIdsSectionBeyondEndOfFile() {
        val fixture = buildDexFixture()
        val bytes = fixture.bytes
        val fileSize = bytes.size
        // move the string_ids section so that it ends two bytes past the end of the file
        bytes.setIntLe(HEADER_STRING_IDS_OFF, fileSize - 2)

        val exception = expectDexException(bytes)

        // the earliest out-of-bounds position is stable: it is exactly the end of the file
        val sectionEnd = fileSize - 2 + fixture.stringIdsSize * 4L
        assertEquals("string_ids section [offset " + (fileSize - 2) + ", end " + sectionEnd +
                ") exceeds file size " + fileSize + " (first out-of-bounds offset: " + fileSize + ")",
                exception.message)
    }

    @Test
    fun rejectOverlappingMapSections() {
        val fixture = buildDexFixture()
        val bytes = fixture.bytes
        // move the string_ids section so that it overlaps the type_ids section
        bytes.setIntLe(HEADER_STRING_IDS_OFF, fixture.typeIdsOff)

        val exception = expectDexException(bytes)

        assertEquals("dex sections overlap: string_ids [" + fixture.typeIdsOff + ", " +
                (fixture.typeIdsOff + fixture.stringIdsSize * 4L) + ") and type_ids [" +
                fixture.typeIdsOff + ", " + (fixture.typeIdsOff + fixture.typeIdsSize * 4L) + ")",
                exception.message)
    }

    @Test
    fun rejectInvalidSuperclassTypeIndex() {
        val fixture = buildDexFixture(superclassIdx = 10)

        val exception = expectDexException(fixture.bytes)

        assertEquals("invalid type_ids index: 10 (type_ids size: 4)", exception.message)
    }

    @Test
    fun rejectInvalidStringIndexInAnnotationType() {
        val fixture = buildDexFixture()
        val bytes = fixture.bytes
        // corrupt the descriptor index of the annotation type (the second type_ids entry)
        bytes.setIntLe(fixture.typeIdsOff + 4, 99)

        val exception = expectDexException(bytes)

        assertEquals("invalid string_ids index: 99 (string_ids size: 5)", exception.message)
    }

    @Test
    fun rejectMethodIndexDeltaWraparound() {
        // the second method_idx_diff wraps the cumulative method index around to a negative value
        val fixture = buildDexFixture(
                methodNames = listOf("testFoo", "testBar"),
                methodIdxDiffs = listOf(1, 0x7FFFFFFF),
                directoryMethodIdxs = listOf(Int.MIN_VALUE))

        val exception = expectDexException(fixture.bytes)

        assertEquals("invalid method_ids index: -2147483648 (method_ids size: 2)", exception.message)
    }

    // endregion

    // region class data failure paths

    @Test
    fun rejectUleb128Overflow() {
        // a method_idx_diff encoded as a five byte LEB128 sequence whose fifth byte
        // carries bits beyond a 32 bit value
        val classData = byteArrayOf(0, 0, 0, 1,
                0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0x7F, 1, 0)
        val fixture = buildDexFixture(classDataBytes = classData)

        val exception = expectDexException(fixture.bytes)

        assertEquals("LEB128 value overflows 32 bits", exception.message)
    }

    @Test
    fun rejectTruncatedUleb128() {
        // virtualMethodsSize = 1, then a method_idx_diff whose continuation bit is set
        // on every byte remaining in the file
        val classData = byteArrayOf(0, 0, 0, 1, 0x80.toByte(), 0x80.toByte(), 0x80.toByte())
        val fixture = buildDexFixture(classDataBytes = classData)

        val exception = expectDexException(fixture.bytes)

        // the sequence runs exactly to the end of the file before the parser gives up
        assertEquals("truncated LEB128 sequence at position " + fixture.bytes.size, exception.message)
    }

    @Test
    fun rejectOversizedDeclaredMethodCount() {
        // virtualMethodsSize = 64 with no bytes left for even a single encoded_method
        val fixture = buildDexFixture(classDataBytes = byteArrayOf(0, 0, 0, 64))

        val exception = expectDexException(fixture.bytes)

        assertEquals("invalid declared virtual methods size: 64 (0 bytes remaining)", exception.message)
    }

    // endregion

    // region encoded value failure paths

    @Test
    fun rejectCorruptEncodedArray() {
        // an annotation element whose array value declares 0x7FFFFFFF elements
        val elementValue = byteArrayOf(0x1C,
                0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0x07)
        val fixture = buildDexFixture(annotationElementValue = elementValue)

        val exception = expectDexException(fixture.bytes)

        // visibility + type index + size + name index + value type + array size = 10 bytes
        val remaining = fixture.bytes.size - (fixture.annotationItemOffs[0] + 10)
        assertEquals("invalid declared encoded array size: 2147483647 ($remaining bytes remaining)",
                exception.message)
    }

    @Test
    fun rejectUnsupportedEncodedValueType() {
        // 0x05 is not a valid value_type
        val fixture = buildDexFixture(annotationElementValue = byteArrayOf(0x05))

        val exception = expectDexException(fixture.bytes)

        assertEquals("Bad value type: 5", exception.message)
    }

    // endregion

    // region annotation directory failure paths

    @Test
    fun rejectAnnotationSetOffsetOutOfBounds() {
        val fixture = buildDexFixture()
        val bytes = fixture.bytes
        // corrupt the annotationsOff of the first method annotation entry in the directory
        bytes.setIntLe(fixture.annotationsDirectoryOff + 16 + 4, Integer.MAX_VALUE)

        val exception = expectDexException(bytes)

        assertEquals("annotation set offset 2147483647 is out of bounds (file size " + bytes.size + ")",
                exception.message)
    }

    @Test
    fun rejectOversizedAnnotationSetSize() {
        val fixture = buildDexFixture()
        val bytes = fixture.bytes
        bytes.setIntLe(fixture.annotationSetOffs[0], 1000)

        val exception = expectDexException(bytes)

        val remaining = bytes.size - (fixture.annotationSetOffs[0] + 4)
        assertEquals("invalid declared annotation set size: 1000 ($remaining bytes remaining)",
                exception.message)
    }

    // endregion

    @Test
    fun distinguishCorruptionCategories() {
        val messages = mutableSetOf<String>()

        // invalid index
        messages.add(expectDexException(buildDexFixture(superclassIdx = 10).bytes).message!!)
        // truncated LEB128
        messages.add(expectDexException(buildDexFixture(
                classDataBytes = byteArrayOf(0, 0, 0, 1, 0x80.toByte(), 0x80.toByte(), 0x80.toByte())).bytes).message!!)
        // declared length too large
        messages.add(expectDexException(buildDexFixture(
                classDataBytes = byteArrayOf(0, 0, 0, 64)).bytes).message!!)
        // unsupported value type
        messages.add(expectDexException(buildDexFixture(
                annotationElementValue = byteArrayOf(0x05)).bytes).message!!)

        assertEquals(4, messages.size)
    }

    // region helpers

    private fun expectDexException(dexBytes: ByteArray): DexException {
        try {
            DexParser.findTestMethods(writeApk(dexBytes), emptyList())
            throw AssertionError("Expected DexException but parsing succeeded")
        } catch (e: DexException) {
            return e
        }
    }

    private fun writeApk(vararg dexFiles: ByteArray): String {
        val apkFile = tempFolder.newFile()
        ZipOutputStream(apkFile.outputStream()).use { zip ->
            dexFiles.forEachIndexed { index, bytes ->
                val entryName = if (index == 0) "classes.dex" else "classes${index + 1}.dex"
                zip.putNextEntry(ZipEntry(entryName))
                zip.write(bytes)
                zip.closeEntry()
            }
        }
        return apkFile.absolutePath
    }

    // endregion
}
