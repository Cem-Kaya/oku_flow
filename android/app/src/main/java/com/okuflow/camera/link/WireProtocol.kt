package com.okuflow.camera.link

import java.io.EOFException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Version 1 framing, shared with scripts/android_camera_probe.py; no allocation before bounds checks. */
object WireProtocol {
    const val VERSION = 1
    const val CONTROL = 1
    const val VIDEO = 2
    const val ACK = 1
    const val COMMAND = 2
    const val STATE = 3
    const val METADATA = 4
    const val ERROR = 5
    const val FRAME = 6
    const val MAX_CONTROL = 64 * 1024
    const val MAX_VIDEO = 16 * 1024 * 1024
    const val KEY_FRAME = 1
    const val CONFIG = 2
    data class Hello(val role: Int, val json: ByteArray)
    data class Packet(val type: Int, val flags: Int, val payload: ByteArray)

    fun readHello(input: InputStream): Hello {
        val bytes = readExact(input, 12)
        require(bytes.copyOfRange(0, 4).contentEquals(byteArrayOf(79, 75, 85, 70))) { "Bad protocol magic" }
        val header = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        require(header.getShort(4).toInt() == VERSION) { "Unsupported protocol version" }
        val role = header.getShort(6).toInt()
        require(role == CONTROL || role == VIDEO) { "Invalid channel" }
        val size = header.getInt(8)
        require(size in 2..MAX_CONTROL) { "Invalid hello length" }
        return Hello(role, readExact(input, size))
    }

    fun readPacket(input: InputStream, limit: Int = MAX_CONTROL): Packet {
        val header = ByteBuffer.wrap(readExact(input, 8)).order(ByteOrder.LITTLE_ENDIAN)
        val type = header.short.toInt() and 0xffff
        val flags = header.short.toInt() and 0xffff
        val size = header.int
        require(size in 0..limit) { "Invalid packet length" }
        return Packet(type, flags, readExact(input, size))
    }

    fun writePacket(output: OutputStream, type: Int, payload: ByteArray, flags: Int = 0) {
        require(payload.size <= if (type == FRAME) MAX_VIDEO else MAX_CONTROL)
        output.write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN)
            .putShort(type.toShort()).putShort(flags.toShort()).putInt(payload.size).array())
        output.write(payload)
        output.flush()
    }

    fun framePayload(ptsUs: Long, sequence: Long, generation: Int, bytes: ByteArray): ByteArray {
        require(bytes.size <= MAX_VIDEO - 20)
        return ByteBuffer.allocate(20 + bytes.size).order(ByteOrder.LITTLE_ENDIAN)
            .putLong(ptsUs).putLong(sequence).putInt(generation).put(bytes).array()
    }

    private fun readExact(input: InputStream, size: Int): ByteArray {
        val bytes = ByteArray(size)
        var position = 0
        while (position < size) {
            val count = input.read(bytes, position, size - position)
            if (count < 0) throw EOFException("Truncated packet")
            if (count == 0) throw EOFException("Empty read")
            position += count
        }
        return bytes
    }
}
