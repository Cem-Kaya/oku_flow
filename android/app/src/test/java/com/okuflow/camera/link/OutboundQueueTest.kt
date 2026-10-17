package com.okuflow.camera.link

import com.okuflow.camera.link.OutboundQueue.Admission
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Test

class OutboundQueueTest {
    private fun packet(type: Int, size: Int, tag: Int = 0) =
        WireProtocol.Packet(type, 0, ByteArray(size).also { if (size > 0) it[0] = tag.toByte() })

    @Test fun advisoryStopsAtPacketHighWaterWhileReliableUsesReserve() {
        val queue = OutboundQueue(capacity = 4, byteLimit = 1_000, reservedPackets = 2)
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertEquals(Admission.Dropped, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, 1)))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.ERROR, 1)))
        // Reliable overflow is still reported for the caller's disconnect policy.
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 1)))
        assertEquals(Admission.Dropped, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertEquals(4, queue.size)
    }

    @Test fun advisoryStopsAtByteHighWaterWhileReliableUsesReserve() {
        val queue = OutboundQueue(capacity = 100, byteLimit = 100, reservedBytes = 40)
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.METADATA, 60), advisory = true))
        assertEquals(Admission.Dropped, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, 40)))
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 1)))
        assertEquals(100, queue.queuedBytes)
    }

    @Test fun reliableAboveHighWaterKeepsAdvisoryOut() {
        val queue = OutboundQueue(capacity = 4, byteLimit = 100, reservedPackets = 1, reservedBytes = 50)
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, 70)))
        // Reliable bytes already exceed the advisory limit: metadata drops, reliable still fits.
        assertEquals(Admission.Dropped, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.ACK, 30)))
    }

    @Test fun saturatedAdvisoryNeverRejectsAndRecoversAfterDrain() {
        val queue = controlQueue()
        var queued = 0
        repeat(10_000) {
            when (queue.offer(packet(WireProtocol.METADATA, 512), advisory = true)) {
                Admission.Queued -> queued++
                Admission.Dropped -> Unit
                Admission.Rejected -> fail("Advisory metadata must never be rejected")
            }
        }
        assertEquals(96, queued)
        // The full control reserve is still available to reliable packets.
        repeat(32) { assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, 1_024))) }
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 1)))
        // Draining one packet admits metadata again only while below the high-water mark.
        repeat(33) { assertNotNull(queue.take()); queue.complete() }
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.METADATA, 512), advisory = true))
    }

    @Test fun controlReserveHoldsTwoMaximumControlPayloads() {
        val queue = controlQueue()
        while (queue.offer(packet(WireProtocol.METADATA, WireProtocol.MAX_CONTROL), advisory = true) == Admission.Queued) { /* Fill the advisory byte allowance. */ }
        assertEquals(6 * WireProtocol.MAX_CONTROL, queue.queuedBytes)
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.ERROR, WireProtocol.MAX_CONTROL)))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, WireProtocol.MAX_CONTROL)))
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 1)))
    }

    @Test fun videoQueueHasNoAdvisoryReserveAndRejectsOverflow() {
        val queue = videoQueue()
        repeat(32) { assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.FRAME, 1))) }
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.FRAME, 1)))
    }

    @Test fun inFlightPacketOwnsBytesUntilCompleteButNotItsQueueSlot() {
        val queue = OutboundQueue(capacity = 1, byteLimit = 10)
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, 8)))
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 1)))
        assertNotNull(queue.take())
        assertEquals(0, queue.size)
        assertEquals(8, queue.queuedBytes)
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 3)))
        assertEquals(Admission.Queued, queue.offer(packet(WireProtocol.STATE, 2)))
        queue.complete()
        assertEquals(2, queue.queuedBytes)
    }

    @Test fun deliveryKeepsFifoOrderAcrossReliableAndAdmittedAdvisoryPackets() {
        val queue = OutboundQueue(capacity = 3, byteLimit = 100, reservedPackets = 1)
        queue.offer(packet(WireProtocol.ACK, 1, tag = 1))
        queue.offer(packet(WireProtocol.METADATA, 1, tag = 2), advisory = true)
        assertEquals(Admission.Dropped, queue.offer(packet(WireProtocol.METADATA, 1, tag = 3), advisory = true))
        queue.offer(packet(WireProtocol.STATE, 1, tag = 4))
        val delivered = List(3) { queue.take()!!.also { queue.complete() }.payload[0].toInt() }
        assertEquals(listOf(1, 2, 4), delivered)
    }

    @Test fun closeWakesBlockedWriterAndRefusesLaterPackets() {
        val queue = OutboundQueue(capacity = 2, byteLimit = 10, reservedPackets = 1)
        val result = AtomicReference<Any?>("pending")
        val waiting = CountDownLatch(1)
        val writer = Thread { waiting.countDown(); result.set(queue.take()) }.apply { isDaemon = true; start() }
        assertTrue(waiting.await(5, TimeUnit.SECONDS))
        queue.close()
        writer.join(5_000)
        assertFalse(writer.isAlive)
        assertNull(result.get())
        assertEquals(Admission.Rejected, queue.offer(packet(WireProtocol.STATE, 1)))
        assertEquals(Admission.Dropped, queue.offer(packet(WireProtocol.METADATA, 1), advisory = true))
        assertNull(queue.take())
    }

    @Test fun closeDuringWriteReleasesOwnershipAndLateCompleteIsHarmless() {
        val queue = OutboundQueue(capacity = 2, byteLimit = 10)
        queue.offer(packet(WireProtocol.STATE, 4))
        queue.offer(packet(WireProtocol.STATE, 4))
        assertNotNull(queue.take())
        queue.close()
        queue.complete()
        assertEquals(0, queue.queuedBytes)
        assertEquals(0, queue.size)
    }

    @Test(expected = IllegalStateException::class) fun writerMustCompleteBeforeTakingAgain() {
        val queue = OutboundQueue(capacity = 2, byteLimit = 10)
        queue.offer(packet(WireProtocol.STATE, 1))
        queue.offer(packet(WireProtocol.STATE, 1))
        queue.take()
        queue.take()
    }

    @Test fun concurrentProducersKeepBoundsAndPerProducerReliableOrder() {
        val producers = 4
        val perProducer = 2_000
        val byteLimit = 4_096
        val queue = OutboundQueue(capacity = 64, byteLimit = byteLimit, reservedPackets = 16, reservedBytes = 1_024)
        val delivered = Collections.synchronizedList(mutableListOf<WireProtocol.Packet>())
        val failure = AtomicReference<Throwable?>(null)
        val start = CountDownLatch(1)
        val writer = Thread {
            try {
                while (true) {
                    val next = queue.take() ?: break
                    if (queue.queuedBytes > byteLimit) error("Byte bound exceeded")
                    delivered += next
                    queue.complete()
                }
            } catch (throwable: Throwable) { failure.compareAndSet(null, throwable) }
        }.apply { isDaemon = true; start() }
        val threads = List(producers) { producer ->
            Thread {
                try {
                    start.await()
                    for (index in 0 until perProducer) {
                        val reliable = index % 2 == 0
                        val payload = java.nio.ByteBuffer.allocate(16).putInt(producer).putInt(index).array()
                        val type = if (reliable) WireProtocol.STATE else WireProtocol.METADATA
                        // Reliable packets wait for space here only to keep the test lossless;
                        // the server itself never retries a rejected reliable packet.
                        while (true) {
                            val admission = queue.offer(WireProtocol.Packet(type, 0, payload), advisory = !reliable)
                            if (queue.queuedBytes > byteLimit || queue.size > 64) error("Queue bound exceeded")
                            if (admission != Admission.Rejected) break
                            Thread.yield()
                        }
                    }
                } catch (throwable: Throwable) { failure.compareAndSet(null, throwable) }
            }.apply { isDaemon = true; start() }
        }
        start.countDown()
        threads.forEach { it.join(30_000) }
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
        while (queue.size > 0 && System.nanoTime() < deadline) Thread.sleep(1)
        queue.close()
        writer.join(5_000)
        failure.get()?.let { throw AssertionError(it) }
        assertTrue(threads.none { it.isAlive })

        val byProducer = delivered.toList().groupBy({ java.nio.ByteBuffer.wrap(it.payload).int }) {
            it.type to java.nio.ByteBuffer.wrap(it.payload).getInt(4)
        }
        for (producer in 0 until producers) {
            val sequence = byProducer.getValue(producer)
            // Every reliable packet arrives exactly once and in order; admitted metadata keeps its place.
            assertEquals((0 until perProducer step 2).toList(),
                sequence.filter { it.first == WireProtocol.STATE }.map { it.second })
            assertEquals(sequence.map { it.second }.sorted(), sequence.map { it.second })
        }
    }
}
