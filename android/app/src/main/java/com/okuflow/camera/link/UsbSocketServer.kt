package com.okuflow.camera.link

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.SystemClock
import android.util.Log
import org.json.JSONObject
import java.util.UUID
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/** Abstract Unix socket reached through adb. Accepts shell/root peers, one control/video pair only. */
class UsbSocketServer(
    private val capabilities: () -> JSONObject,
    private val onCommand: (JSONObject) -> Unit,
    private val onConnect: () -> Unit,
    private val onDisconnect: () -> Unit,
) : AutoCloseable {
    private val server = LocalServerSocket("okuflow_camera")
    private val running = AtomicBoolean(true)
    private val sockets = ConcurrentHashMap.newKeySet<LocalSocket>()
    private val lock = Any()
    private var session: Session? = null

    private inner class Channel(val socket: LocalSocket, capacity: Int, private val byteLimit: Int) {
        private val queue = ArrayBlockingQueue<WireProtocol.Packet>(capacity)
        private var queuedBytes = 0
        @Volatile var writeStarted = 0L
        @Volatile var closed = false
        init {
            thread(name = "OkuFlow-USB-write", isDaemon = true) {
                try {
                    while (!closed && running.get()) {
                        val packet = queue.take()
                        writeStarted = SystemClock.elapsedRealtime()
                        WireProtocol.writePacket(socket.outputStream, packet.type, packet.payload, packet.flags)
                        writeStarted = 0
                        synchronized(this) { queuedBytes -= packet.payload.size }
                    }
                } catch (exception: Exception) {
                    if (!closed) Log.w("OkuFlow-USB", "Channel write failed: ${exception.javaClass.simpleName}")
                    disconnect(socket)
                }
            }
        }
        @Synchronized fun send(packet: WireProtocol.Packet): Boolean {
            if (closed || queuedBytes + packet.payload.size > byteLimit || !queue.offer(packet)) return false
            queuedBytes += packet.payload.size
            return true
        }
        fun close() {
            closed = true
            runCatching { socket.close() }
            // Wake a writer blocked on take; socket closure wakes a blocked native write.
            queue.clear()
            queue.offer(WireProtocol.Packet(0, 0, byteArrayOf()))
        }
    }
    private inner class Session(val control: Channel, val codecs: Set<String>, val limits: HostLimits) {
        val token = UUID.randomUUID().toString()
        var video: Channel? = null
        var generation = -1
    }

    init {
        thread(name = "OkuFlow-USB-accept", isDaemon = true) {
            while (running.get()) {
                try {
                    val socket = server.accept()
                    val uid = socket.peerCredentials.uid
                    if ((uid != 2000 && uid != 0) || sockets.size >= 4) { socket.close(); continue }
                    sockets.add(socket)
                    thread(name = "OkuFlow-USB-read", isDaemon = true) { readChannel(socket) }
                } catch (_: Exception) { if (running.get()) close() }
            }
        }
        thread(name = "OkuFlow-USB-watchdog", isDaemon = true) {
            while (running.get()) {
                Thread.sleep(500)
                val stalled = synchronized(lock) {
                    listOfNotNull(session?.control, session?.video).filter {
                        // Read once: a completed write can reset this volatile field
                        // between reads, making uptime - 0 look like a stalled write.
                        val startedAt = it.writeStarted
                        startedAt != 0L && SystemClock.elapsedRealtime() - startedAt > 3000
                    }.map { it.socket }
                }
                if (stalled.isNotEmpty()) Log.w("OkuFlow-USB", "Disconnecting stalled channel writer")
                stalled.forEach { disconnect(it) }
            }
        }
    }

    private fun readChannel(socket: LocalSocket) {
        try {
            socket.soTimeout = 5000
            val hello = WireProtocol.readHello(socket.inputStream)
            val json = JSONObject(hello.json.toString(Charsets.UTF_8))
            if (hello.role == WireProtocol.CONTROL) {
                val codecs = requireNotNull(json.optJSONArray("codecs")) { "Missing host codecs" }
                val supported = (0 until codecs.length()).map { codecs.getString(it) }.filter { it in listOf("avc", "hevc") }.toSet()
                require(supported.isNotEmpty()) { "No shared codec" }
                val created = synchronized(lock) {
                    require(session == null) { "Another host is already connected" }
                    val limits = HostLimits(json.optInt("maxWidth", 16384).coerceIn(64, 16384),
                        json.optInt("maxHeight", 16384).coerceIn(48, 16384),
                        json.optLong("maxPixelsPerSecond", Long.MAX_VALUE).coerceAtLeast(1))
                    Session(Channel(socket, 128, 512 * 1024), supported, limits).also { session = it }
                }
                send(created.control, WireProtocol.ACK, capabilities().put("token", created.token)
                    .put("protocol", WireProtocol.VERSION).put("hostCodecs", org.json.JSONArray(supported.toList())))
                socket.soTimeout = 30_000 // Hosts send a ping at least every 10 seconds.
                while (running.get()) {
                    val packet = WireProtocol.readPacket(socket.inputStream)
                    require(packet.type == WireProtocol.COMMAND && packet.flags == 0) { "Invalid control packet" }
                    val command = JSONObject(packet.payload.toString(Charsets.UTF_8))
                    if (command.optString("op") == "ping") {
                        send(created.control, WireProtocol.STATE, JSONObject().put("event", "pong").put("id", command.optLong("id")))
                    } else {
                        require(synchronized(lock) { session === created && created.video != null }) { "Video channel is not connected" }
                        onCommand(command)
                    }
                }
            } else {
                val bound = synchronized(lock) {
                    val current = requireNotNull(session) { "Connect the control channel first" }
                    require(current.token == json.optString("token") && current.video == null) { "Invalid video binding" }
                    current.video = Channel(socket, 32, 32 * 1024 * 1024)
                    current.video!!
                }
                // Publish Connected before the ACK lets the host request its first frame.
                onConnect()
                send(bound, WireProtocol.ACK, JSONObject().put("channel", "video"))
                socket.soTimeout = 0
                // Video is unidirectional. EOF detects disconnect even while the camera is idle.
                require(socket.inputStream.read() == -1) { "Unexpected video input" }
            }
        } catch (exception: Exception) {
            Log.i("OkuFlow-USB", "Channel read ended: ${exception.javaClass.simpleName}")
            // Invalid/unauthorized peers are closed; errors do not reach another host session.
        } finally { disconnect(socket) }
    }

    fun hostCodecs(): Set<String> = synchronized(lock) { session?.codecs ?: emptySet() }
    fun hostLimits(): HostLimits = synchronized(lock) { session?.limits ?: HostLimits() }
    fun connected(): Boolean = synchronized(lock) { session?.video != null }
    fun state(json: JSONObject): Boolean {
        val accepted = synchronized(lock) {
            val current = session ?: return@synchronized false
            if (json.optString("event") == "configured") current.generation = json.getInt("generation")
            !json.has("generation") || current.generation == json.getInt("generation")
        }
        if (accepted) control(WireProtocol.STATE, json)
        return accepted
    }
    fun metadata(json: JSONObject) {
        if (synchronized(lock) { session?.generation == json.optInt("generation", -2) }) control(WireProtocol.METADATA, json)
    }
    fun error(message: String) = control(WireProtocol.ERROR, JSONObject().put("error", message))

    private fun control(type: Int, json: JSONObject) {
        val channel = synchronized(lock) { session?.control } ?: return
        send(channel, type, json)
    }
    private fun send(channel: Channel, type: Int, json: JSONObject) {
        val bytes = json.toString().toByteArray(Charsets.UTF_8)
        if (bytes.size > WireProtocol.MAX_CONTROL || !channel.send(WireProtocol.Packet(type, 0, bytes))) {
            Log.w("OkuFlow-USB", "Control queue limit reached")
            disconnect(channel.socket)
        }
    }

    fun frame(pts: Long, sequence: Long, generation: Int, flags: Int, bytes: ByteArray) {
        val channel = synchronized(lock) { session?.takeIf { it.generation == generation }?.video } ?: return
        if (!channel.send(WireProtocol.Packet(WireProtocol.FRAME, flags,
                WireProtocol.framePayload(pts, sequence, generation, bytes)))) {
            Log.w("OkuFlow-USB", "Video queue limit reached")
            disconnect(channel.socket)
        }
    }

    private fun disconnect(socket: LocalSocket) {
        val retired = synchronized(lock) {
            val current = session
            if (current != null && (current.control.socket === socket || current.video?.socket === socket)) {
                session = null
                current
            } else null
        }
        if (retired != null) {
            retired.control.close()
            retired.video?.close()
            sockets.remove(retired.control.socket)
            retired.video?.let { sockets.remove(it.socket) }
            onDisconnect()
        }
        sockets.remove(socket)
        runCatching { socket.close() }
    }

    override fun close() {
        if (!running.getAndSet(false)) return
        runCatching { server.close() }
        sockets.toList().forEach { disconnect(it) }
    }
}

data class HostLimits(val width: Int = 16384, val height: Int = 16384, val pixelsPerSecond: Long = Long.MAX_VALUE)
