package com.okuflow.camera.link

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.*
import org.junit.Test

class WireProtocolTest {
    @Test fun goldenPacketMatchesHost() {
        val out = ByteArrayOutputStream()
        WireProtocol.writePacket(out, WireProtocol.COMMAND, byteArrayOf(123, 125))
        assertArrayEquals(byteArrayOf(2, 0, 0, 0, 2, 0, 0, 0, 123, 125), out.toByteArray())
        assertEquals(2, WireProtocol.readPacket(ByteArrayInputStream(out.toByteArray())).type)
    }
    @Test fun goldenFramePrefixMatchesHost() {
        val data = WireProtocol.framePayload(1000, 7, 2, byteArrayOf(0, 0, 0, 1, 101))
        val input = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(1000L, input.long)
        assertEquals(7L, input.long)
        assertEquals(2, input.int)
        assertEquals(25, data.size)
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsOversizedBeforeReadingPayload() {
        WireProtocol.readPacket(ByteArrayInputStream(byteArrayOf(2, 0, 0, 0, 1, 0, 1, 0)))
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsNegativeSize() {
        WireProtocol.readPacket(ByteArrayInputStream(byteArrayOf(2, 0, 0, 0, -1, -1, -1, -1)))
    }
    @Test(expected = EOFException::class) fun rejectsTruncatedBody() {
        WireProtocol.readPacket(ByteArrayInputStream(byteArrayOf(2, 0, 0, 0, 2, 0, 0, 0, 123)))
    }
    @Test fun validatesHello() {
        val hello = byteArrayOf(79, 75, 85, 70, 1, 0, 1, 0, 2, 0, 0, 0, 123, 125)
        assertEquals(WireProtocol.CONTROL, WireProtocol.readHello(ByteArrayInputStream(hello)).role)
    }
    @Test(expected = IllegalArgumentException::class) fun rejectsVersionMismatch() {
        WireProtocol.readHello(ByteArrayInputStream(byteArrayOf(79, 75, 85, 70, 2, 0, 1, 0, 2, 0, 0, 0)))
    }
}
