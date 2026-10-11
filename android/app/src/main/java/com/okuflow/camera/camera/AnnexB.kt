package com.okuflow.camera.camera

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer

/** Keeps start-code NALs; converts four-byte length-prefixed encoder output when needed. */
fun annexB(bytes: ByteArray): ByteArray {
    if (bytes.size >= 3 && bytes[0] == 0.toByte() && bytes[1] == 0.toByte() &&
        (bytes[2] == 1.toByte() || (bytes.size >= 4 && bytes[2] == 0.toByte() && bytes[3] == 1.toByte()))) return bytes
    val input = ByteBuffer.wrap(bytes)
    val output = ByteArrayOutputStream(bytes.size + 16)
    while (input.hasRemaining()) {
        require(input.remaining() >= 4) { "Truncated NAL length" }
        val length = input.int
        require(length > 0 && length <= input.remaining()) { "Invalid NAL length" }
        val nal = ByteArray(length)
        input.get(nal)
        output.write(byteArrayOf(0, 0, 0, 1))
        output.write(nal)
    }
    return output.toByteArray()
}
