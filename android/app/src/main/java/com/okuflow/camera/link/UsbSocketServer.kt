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

/**
 * Abstract Unix socket reached through adb. Accepts shell/root peers, one control/video pair only.
 * [problem] supplies the current problem code for non-claiming probes; [onListenerFailed]
 * runs once if the listening socket itself dies so the owner can restart it.
 */
class UsbSocketServer(
    private val capabilities: () -> JSONObject,
    private val onCommand: (String, JSONObject) -> Unit,
    private val onConnect: (String) -> Unit,
    private val problem: () -> String? = { null },
    private val onListenerFailed: () -> Unit = {},
    private val onDisconnect: (String) -> Unit,
) : AutoCloseable {
    private val server = LocalServerSocket("okuflow_camera")
    private val running = AtomicBoolean(true)
    private val sockets = ConcurrentHashMap.newKeySet<LocalSocket>()
    private val lock = Any()
    private var session: Session? = null
    // Camera characteristics and codec lists are slow on a cold process; build once per listener.
    @Volatile private var cachedCapabilities: String? = null

    /** A fresh copy: callers add per-connection fields. */
    private fun capabilityJson(): JSONObject {
        cachedCapabilities?.let { return JSONObject(it) }
        val fresh = capabilities()
        cachedCapabilities = fresh.toString()
        return fresh
    }

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
        val createdAt = SystemClock.elapsedRealtime()
        var video: Channel? = null
        var generation = -1
    }

    init {
        thread(name = "OkuFlow-USB-capabilities", isDaemon = true) {
            runCatching { capabilityJson() }.onFailure { Log.w("OkuFlow-USB", "Capability warm-up failed") }
        }
        thread(name = "OkuFlow-USB-accept", isDaemon = true) {
            while (running.get()) {
                val socket = try { server.accept() } catch (_: Exception) {
                    // Only a dead listening socket ends the loop; the owner restarts the listener.
                    if (running.get()) { close(); onListenerFailed() }
                    break
                }
                // A peer that exits before credentials are read affects only its own socket.
                val uid = runCatching { socket.peerCredentials.uid }.getOrNull()
                if (!peerAllowed(uid) || sockets.size >= 4 || !running.get()) { runCatching { socket.close() }; continue }
                sockets.add(socket)
                runCatching { thread(name = "OkuFlow-USB-read", isDaemon = true) { readChannel(socket) } }
                    .onFailure { disconnect(socket) }
            }
        }
        thread(name = "OkuFlow-USB-watchdog", isDaemon = true) {
            while (running.get()) {
                Thread.sleep(500)
                val now = SystemClock.elapsedRealtime()
                val stalled = synchronized(lock) {
                    val current = session
                    val writers = listOfNotNull(current?.control, current?.video).filter {
                        // Read once: a completed write can reset this volatile field
                        // between reads, making uptime - 0 look like a stalled write.
                        val startedAt = it.writeStarted
                        startedAt != 0L && now - startedAt > 3000
                    }.map { it.socket }
                    val unbound = current?.takeIf { videoBindExpired(it.createdAt, now, it.video != null) }
                    writers + listOfNotNull(unbound?.control?.socket)
                }
                if (stalled.isNotEmpty()) Log.w("OkuFlow-USB", "Disconnecting stalled or unbound channel")
                stalled.forEach { disconnect(it) }
            }
        }
    }

    private fun readChannel(socket: LocalSocket) {
        try {
            socket.soTimeout = 5000
            val hello = WireProtocol.readHello(socket.inputStream)
            val json = JSONObject(hello.json.toString(Charsets.UTF_8))
            if (hello.role == WireProtocol.CONTROL && json.optBoolean("probe", false)) {
                // Discovery: report readiness without claiming the session slot or allocating a token.
                val busy = synchronized(lock) { session != null }
                val reply = capabilityJson().put("protocol", WireProtocol.VERSION).put("probe", true)
                    .put("busy", busy).put("problem", problem() ?: JSONObject.NULL)
                writeDirect(socket, WireProtocol.ACK, reply)
                return
            }
            if (hello.role == WireProtocol.CONTROL) {
                val codecs = requireNotNull(json.optJSONArray("codecs")) { "Missing host codecs" }
                val supported = (0 until codecs.length()).map { codecs.getString(it) }.filter { it in listOf("avc", "hevc") }.toSet()
                require(supported.isNotEmpty()) { "No shared codec" }
                val limits = HostLimits(json.optInt("maxWidth", 16384).coerceIn(64, 16384),
                    json.optInt("maxHeight", 16384).coerceIn(48, 16384),
                    json.optLong("maxPixelsPerSecond", Long.MAX_VALUE).coerceAtLeast(1))
                // Build the ACK before claiming: the bind deadline must not include a cold capability scan.
                val ack = capabilityJson()
                val created = synchronized(lock) {
                    if (session != null) null
                    else Session(Channel(socket, 128, 512 * 1024), supported, limits).also { session = it }
                }
                if (created == null) {
                    writeDirect(socket, WireProtocol.ERROR, JSONObject()
                        .put("error", "Another host is already connected").put("code", SESSION_BUSY_CODE))
                    return
                }
                send(created.control, WireProtocol.ACK, ack.put("token", created.token)
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
                        onCommand(created.token, command)
                    }
                }
            } else {
                val bound = synchronized(lock) {
                    val current = requireNotNull(session) { "Connect the control channel first" }
                    require(current.token == json.optString("token") && current.video == null) { "Invalid video binding" }
                    current.video = Channel(socket, 32, 32 * 1024 * 1024)
                    current

                }
                // No service callback runs under the socket lock. Its serial owner checks
                // this immutable token again; delayed old callbacks cannot affect a successor.
                onConnect(bound.token)
                send(bound.video!!, WireProtocol.ACK, JSONObject().put("channel", "video"))
                socket.soTimeout = 0
                // Video is unidirectional. EOF detects disconnect even while the camera is idle.
                require(socket.inputStream.read() == -1) { "Unexpected video input" }
            }
        } catch (exception: Exception) {
            Log.i("OkuFlow-USB", "Channel read ended: ${exception.javaClass.simpleName}")
            // Invalid/unauthorized peers are closed; errors do not reach another host session.
        } finally { disconnect(socket) }
    }

    fun hostCodecs(id: String): Set<String> = synchronized(lock) { session?.takeIf { it.token == id }?.codecs ?: emptySet() }
    fun hostLimits(id: String): HostLimits = synchronized(lock) { session?.takeIf { it.token == id }?.limits ?: HostLimits() }
    fun connected(id: String): Boolean = synchronized(lock) { session?.let { it.token == id && it.video != null } == true }
    fun connected(): Boolean = synchronized(lock) { session?.video != null }
    fun invalidateStream(id: String) = synchronized(lock) {
        session?.takeIf { it.token == id }?.generation = -1
    }
    fun state(id: String, json: JSONObject): Boolean {
        val channel = synchronized(lock) {
            val current = session?.takeIf { it.token == id && it.video != null } ?: return@synchronized null
            if (json.optString("event") == "configured") current.generation = json.getInt("generation")
            current.control.takeIf { !json.has("generation") || current.generation == json.getInt("generation") }
        } ?: return false
        send(channel, WireProtocol.STATE, json)
        return true
    }
    fun metadata(id: String, json: JSONObject) {
        val channel = synchronized(lock) {
            session?.takeIf { it.token == id && it.generation == json.optInt("generation", -2) }?.control
        } ?: return
        send(channel, WireProtocol.METADATA, json)
    }
    fun error(id: String, message: String, code: String? = null) =
        control(id, WireProtocol.ERROR, JSONObject().put("error", message).apply { if (code != null) put("code", code) })

    /** One small reply on a connection that never joins the session; it fits the socket send buffer. */
    private fun writeDirect(socket: LocalSocket, type: Int, json: JSONObject) {
        runCatching { WireProtocol.writePacket(socket.outputStream, type, json.toString().toByteArray(Charsets.UTF_8)) }
    }

    private fun control(id: String, type: Int, json: JSONObject) {
        val channel = synchronized(lock) { session?.takeIf { it.token == id }?.control } ?: return
        send(channel, type, json)
    }
    private fun send(channel: Channel, type: Int, json: JSONObject) {
        val bytes = json.toString().toByteArray(Charsets.UTF_8)
        if (bytes.size > WireProtocol.MAX_CONTROL || !channel.send(WireProtocol.Packet(type, 0, bytes))) {
            Log.w("OkuFlow-USB", "Control queue limit reached")
            disconnect(channel.socket)
        }
    }

    fun frame(id: String, pts: Long, sequence: Long, generation: Int, flags: Int, bytes: ByteArray) {
        val channel = synchronized(lock) { session?.takeIf { it.token == id && it.generation == generation }?.video } ?: return
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
            runCatching { onDisconnect(retired.token) }.onFailure { Log.w("OkuFlow-USB", "Session cleanup failed", it) }
            retired.control.close()
            retired.video?.close()
            sockets.remove(retired.control.socket)
            retired.video?.let { sockets.remove(it.socket) }
        }
        sockets.remove(socket)
        runCatching { socket.close() }
    }

    /** Retire capture after a debounced USB removal without closing the reusable listener. */
    fun disconnectHost() {
        val socket = synchronized(lock) { session?.control?.socket } ?: return
        disconnect(socket)
    }

    override fun close() {
        if (!running.getAndSet(false)) return
        runCatching { server.close() }
        sockets.toList().forEach { disconnect(it) }
    }
}

data class HostLimits(val width: Int = 16384, val height: Int = 16384, val pixelsPerSecond: Long = Long.MAX_VALUE)
