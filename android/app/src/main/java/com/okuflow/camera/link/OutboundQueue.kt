package com.okuflow.camera.link

import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Control: 128 packets / 512 KiB. Metadata stops at 96 packets / 384 KiB, holding 32 packets
 * and two maximum-size control payloads for ACK/STATE/ERROR.
 */
internal fun controlQueue() = OutboundQueue(128, 512 * 1024, reservedPackets = 32, reservedBytes = 2 * WireProtocol.MAX_CONTROL)

/** Video: 32 packets / 32 MiB, all reliable; overflow still retires the session. */
internal fun videoQueue() = OutboundQueue(32, 32 * 1024 * 1024)

/**
 * Bounded FIFO between socket producers and one writer thread. [capacity] counts queued
 * packets; [byteLimit] also counts the packet the writer owns until [complete]. Reliable
 * packets may use the whole bound; [Admission.Rejected] is the caller's failure to handle.
 * Advisory packets stop at a high-water mark below [reservedPackets]/[reservedBytes] and are
 * dropped there, so they never consume capacity held for reliable packets.
 * The lock covers only queue state; no I/O or callback runs under it.
 */
internal class OutboundQueue(
    private val capacity: Int,
    private val byteLimit: Int,
    reservedPackets: Int = 0,
    reservedBytes: Int = 0,
) {
    enum class Admission { Queued, Dropped, Rejected }

    private val advisoryCapacity = capacity - reservedPackets
    private val advisoryByteLimit = byteLimit - reservedBytes
    private val lock = ReentrantLock()
    private val available = lock.newCondition()
    private val pending = ArrayDeque<WireProtocol.Packet>()
    private var inFlight: WireProtocol.Packet? = null
    private var bytes = 0
    private var closed = false

    init {
        require(reservedPackets in 0 until capacity && reservedBytes in 0 until byteLimit)
    }

    val size: Int get() = lock.withLock { pending.size }
    /** Queued plus in-flight payload bytes. */
    val queuedBytes: Int get() = lock.withLock { bytes }

    fun offer(packet: WireProtocol.Packet, advisory: Boolean = false): Admission = lock.withLock {
        val packetLimit = if (advisory) advisoryCapacity else capacity
        val bytesLimit = if (advisory) advisoryByteLimit else byteLimit
        if (closed || pending.size >= packetLimit || packet.payload.size > bytesLimit - bytes) {
            if (advisory) Admission.Dropped else Admission.Rejected
        } else {
            pending.addLast(packet)
            bytes += packet.payload.size
            available.signal()
            Admission.Queued
        }
    }

    /** Single writer: blocks for the next packet, or returns null once closed. Call [complete] after writing it. */
    fun take(): WireProtocol.Packet? = lock.withLock {
        check(inFlight == null) { "Previous packet was not completed" }
        while (!closed && pending.isEmpty()) available.await()
        if (closed) null else pending.removeFirst().also { inFlight = it }
    }

    /** Releases the in-flight packet's bytes after its write finished. */
    fun complete() = lock.withLock {
        inFlight?.let { bytes -= it.payload.size }
        inFlight = null
    }

    /** Discards queued packets and wakes the writer; later offers fail. */
    fun close() = lock.withLock {
        closed = true
        pending.clear()
        inFlight = null
        bytes = 0
        available.signalAll()
    }
}
