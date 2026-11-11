/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import com.linkedin.dex.parser.ParseUtils
import java.nio.ByteBuffer
import java.nio.ByteOrder

class DexFile(byteBuffer: ByteBuffer) {
    val byteBuffer: ByteBuffer
    val headerItem: HeaderItem
    val stringIds: Array<StringIdItem>
    val typeIds: Array<TypeIdItem>
    val protoIds: Array<ProtoIdItem>
    val fieldIds: Array<FieldIdItem>
    val methodIds: Array<MethodIdItem>
    val classDefs: Array<ClassDefItem>

    companion object {
        val NO_INDEX = -1
        val HEADER_SIZE = 0x70
    }

    inline fun <reified T> parse(count: Int, offset: Int, size: Int, init: (ByteBuffer) -> T): Array<T> {
        return Array(count, { index ->
            byteBuffer.position(offset + (index * size))
            init(byteBuffer)
        })
    }

    init {
        this.byteBuffer = byteBuffer.asReadOnlyBuffer().order(ByteOrder.LITTLE_ENDIAN)
        this.byteBuffer.position(0)
        if (this.byteBuffer.capacity() < HEADER_SIZE) {
            throw DexException("dex file too small: " + this.byteBuffer.capacity() + " bytes (header requires " + HEADER_SIZE + " bytes)")
        }
        headerItem = HeaderItem(this.byteBuffer)
        headerItem.validate()
        validateTableSections()
        stringIds = parse(headerItem.stringIdsSize, headerItem.stringIdsOff, StringIdItem.size) { StringIdItem(it) }
        typeIds = parse(headerItem.typeIdsSize, headerItem.typeIdsOff, TypeIdItem.size) { TypeIdItem(it) }
        protoIds = parse(headerItem.protoIdsSize, headerItem.protoIdsOff, ProtoIdItem.size) { ProtoIdItem(it) }
        fieldIds = parse(headerItem.fieldIdsSize, headerItem.fieldIdsOff, FieldIdItem.size) { FieldIdItem(it) }
        methodIds = parse(headerItem.methodIdsSize, headerItem.methodIdsOff, MethodIdItem.size) { MethodIdItem(it) }
        classDefs = parse(headerItem.classDefsSize, headerItem.classDefsOff, ClassDefItem.size) { ClassDefItem(it) }
    }

    /**
     * Verifies that every table declared in the header lies inside the file and that no two sections
     * (including the header itself) overlap. The first out-of-bounds offset is reported so that
     * corruption is always caught at a stable, deterministic position.
     */
    private fun validateTableSections() {
        val sections = listOf(
                TableSection("string_ids", headerItem.stringIdsOff, headerItem.stringIdsSize, StringIdItem.size),
                TableSection("type_ids", headerItem.typeIdsOff, headerItem.typeIdsSize, TypeIdItem.size),
                TableSection("proto_ids", headerItem.protoIdsOff, headerItem.protoIdsSize, ProtoIdItem.size),
                TableSection("field_ids", headerItem.fieldIdsOff, headerItem.fieldIdsSize, FieldIdItem.size),
                TableSection("method_ids", headerItem.methodIdsOff, headerItem.methodIdsSize, MethodIdItem.size),
                TableSection("class_defs", headerItem.classDefsOff, headerItem.classDefsSize, ClassDefItem.size))

        val fileSize = byteBuffer.capacity()
        sections.forEach { section ->
            if (section.count < 0) {
                throw DexException("negative " + section.name + " size: " + section.count)
            }
            if (section.count == 0) {
                return@forEach
            }
            if (section.offset < 0 || section.end > fileSize) {
                val firstOutOfBoundsOffset = maxOf(section.offset.toLong(), fileSize.toLong())
                throw DexException(section.name + " section [offset " + section.offset + ", end " + section.end +
                        ") exceeds file size " + fileSize + " (first out-of-bounds offset: " + firstOutOfBoundsOffset + ")")
            }
        }

        val occupiedSections = (listOf(TableSection("header", 0, 1, HEADER_SIZE)) + sections.filter { it.count > 0 })
                .sortedBy { it.offset }
        occupiedSections.zipWithNext().forEach { (first, second) ->
            if (first.end > second.offset) {
                throw DexException("dex sections overlap: " + first.name + " [" + first.offset + ", " + first.end +
                        ") and " + second.name + " [" + second.offset + ", " + second.end + ")")
            }
        }
    }

    private data class TableSection(val name: String, val offset: Int, val count: Int, val itemSize: Int) {
        val end: Long
            get() = offset.toLong() + count.toLong() * itemSize
    }

    val inheritedAnnotationTypeIdIndex: Int? by lazy {
        var result: Int? = null
        typeIds.forEachIndexed { index, typeIdItem ->
            if (ParseUtils.parseDescriptor(byteBuffer, typeIdItem, stringIds) == "Ljava/lang/annotation/Inherited;") {
                result = index
            }
        }

        result
    }

    val typeIdToClassDefMap: Map<Int, ClassDefItem> by lazy {
        val map = mutableMapOf<Int, ClassDefItem>()

        for (classDef in classDefs) {
            map[classDef.classIdx] = classDef
        }
        map.toMap()
    }
}
