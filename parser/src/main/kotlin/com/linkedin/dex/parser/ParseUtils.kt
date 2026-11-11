/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.parser

import com.linkedin.dex.spec.ClassDefItem
import com.linkedin.dex.spec.DexException
import com.linkedin.dex.spec.Leb128
import com.linkedin.dex.spec.MethodIdItem
import com.linkedin.dex.spec.StringIdItem
import com.linkedin.dex.spec.TypeIdItem
import java.nio.ByteBuffer
import java.util.ArrayList

/**
 * Utility methods for parsing different types of data out of a dex file.
 */
object ParseUtils {
    /**
     * Moves the buffer to [offset], throwing a [DexException] instead of an unchecked exception
     * if the offset lies outside the file
     */
    fun checkedPosition(byteBuffer: ByteBuffer, offset: Int, what: String) {
        if (offset < 0 || offset > byteBuffer.limit()) {
            throw DexException(what + " offset " + offset + " is out of bounds (file size " + byteBuffer.limit() + ")")
        }
        byteBuffer.position(offset)
    }

    /**
     * Returns the element at [index], throwing a [DexException] instead of an unchecked exception
     * if the index lies outside the table
     */
    fun <T> Array<T>.checkedGet(index: Int, tableName: String): T {
        if (index < 0 || index >= size) {
            throw DexException("invalid " + tableName + " index: " + index + " (" + tableName + " size: " + size + ")")
        }
        return this[index]
    }

    fun parseClassName(byteBuffer: ByteBuffer, classDefItem: ClassDefItem,
                       typeIds: Array<TypeIdItem>, stringIds: Array<StringIdItem>): String {
        val typeIdItem = typeIds.checkedGet(classDefItem.classIdx, "type_ids")
        return parseDescriptor(byteBuffer, typeIdItem, stringIds)
    }

    fun parseDescriptor(byteBuffer: ByteBuffer, typeIdItem: TypeIdItem, stringIds: Array<StringIdItem>): String {
        val descriptorIdx = typeIdItem.descriptorIdx
        val descriptorId = stringIds.checkedGet(descriptorIdx, "string_ids")
        checkedPosition(byteBuffer, descriptorId.stringDataOff, "string data")
        // read past unused descriptorSize item
        Leb128.readUnsignedLeb128(byteBuffer)
        val encodedDescriptorName = parseStringBytes(byteBuffer)
        return encodedDescriptorName
    }

    fun parseMethodName(byteBuffer: ByteBuffer, stringIds: Array<StringIdItem>, methodId: MethodIdItem): String {
        val methodNameStringId = stringIds.checkedGet(methodId.nameIdx, "string_ids")
        checkedPosition(byteBuffer, methodNameStringId.stringDataOff, "string data")
        // read past unused size item
        Leb128.readUnsignedLeb128(byteBuffer)
        val encodedName = parseStringBytes(byteBuffer)
        return encodedName
    }

    fun parseValueName(byteBuffer: ByteBuffer, stringIds: Array<StringIdItem>, nameIdx: Int): String {
        val stringId = stringIds.checkedGet(nameIdx, "string_ids")
        checkedPosition(byteBuffer, stringId.stringDataOff, "string data")
        // read past unused size item
        Leb128.readUnsignedLeb128(byteBuffer)
        val encodedName = parseStringBytes(byteBuffer)
        return encodedName
    }

    fun parseStringBytes(byteBuffer: ByteBuffer): String {
        val byteList = ArrayList<Byte>()
        do {
            if (!byteBuffer.hasRemaining()) {
                throw DexException("unterminated string at position " + byteBuffer.position())
            }
            val nextByte = byteBuffer.get()
            byteList.add(nextByte)
        } while (nextByte != 0.toByte())
        byteList.removeAt(byteList.size - 1)

        return String(byteList.toByteArray())
    }

    fun parseByteList(byteBuffer: ByteBuffer, length: Int): List<Byte> {
        val tmpArray = ByteArray(length)
        byteBuffer.get(tmpArray, 0, length)
        return tmpArray.toList()
    }
}
