package com.okuflow.camera.link

import android.content.Context
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.util.Log
import com.okuflow.camera.settings.StreamingIntentStore
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

/**
 * Status-only answer while the user has paused streaming: the camera service
 * and its listener are stopped, so without this the computer only sees an
 * absent socket and cannot tell "paused" from "app not running". It binds the
 * same abstract socket, answers non-claiming probes with `problem: "paused"`
 * and refuses control hellos with ERROR code `paused`. It never opens the
 * camera, never allocates a session and never clears Pause.
 */
class PausedStatusListener : PausedStatusHandle {
    private val server = LocalServerSocket(SOCKET_NAME)
    private val running = AtomicBoolean(true)

    /** False once the accept loop ended (an accept failure, or [close]). */
    override val alive: Boolean get() = running.get()

    init {
        thread(name = "OkuFlow-USB-paused", isDaemon = true) {
            while (running.get()) {
                val socket = try {
                    server.accept()
                } catch (exception: Exception) {
                    // The listener reports its end and frees the socket name,
                    // so the supervisor binds a new one on its next tick.
                    if (running.getAndSet(false)) {
                        Log.i("OkuFlow-USB", "Paused status listener ended: ${exception.javaClass.simpleName}")
                        runCatching { server.close() }
                    }
                    break
                }
                runCatching { thread(name = "OkuFlow-USB-paused-reply", isDaemon = true) { answer(socket) } }
                    .onFailure { runCatching { socket.close() } }
            }
        }
    }

    private fun answer(socket: LocalSocket) {
        try {
            val uid = runCatching { socket.peerCredentials.uid }.getOrNull()
            if (!peerAllowed(uid)) return
            socket.soTimeout = 2_000
            val hello = WireProtocol.readHello(socket.inputStream)
            val json = JSONObject(hello.json.toString(Charsets.UTF_8))
            if (hello.role == WireProtocol.CONTROL && json.optBoolean("probe", false)) {
                val reply = JSONObject().put("protocol", WireProtocol.VERSION).put("probe", true)
                    .put("busy", false).put("problem", PAUSED_CODE).put("cameras", JSONArray())
                WireProtocol.writePacket(socket.outputStream, WireProtocol.ACK, reply.toString().toByteArray(Charsets.UTF_8))
            } else {
                val refusal = JSONObject().put("error", "Streaming is paused on the phone").put("code", PAUSED_CODE)
                WireProtocol.writePacket(socket.outputStream, WireProtocol.ERROR, refusal.toString().toByteArray(Charsets.UTF_8))
            }
        } catch (exception: Exception) {
            Log.i("OkuFlow-USB", "Paused status reply ended: ${exception.javaClass.simpleName}")
        } finally {
            runCatching { socket.close() }
        }
    }

    override fun close() {
        if (!running.getAndSet(false)) return
        runCatching { server.close() }
    }
}

/** A bound paused status listener; [alive] turns false when it ended by itself. */
interface PausedStatusHandle : AutoCloseable {
    val alive: Boolean
}

/**
 * Pure binding policy of the paused status listener, driven by [tick]. While
 * Pause is saved a listener is kept bound, retried on every tick for as long
 * as Pause stays saved (the camera service releases the socket
 * asynchronously); otherwise it is closed. A listener that ended by itself
 * (its accept loop failed) is closed and bound again on the next tick, so
 * the computer keeps telling "paused" from "app not running".
 * [releaseForResume] closes it synchronously before Resume starts the camera
 * service, so the two never race for the socket name, and lets the next tick
 * bind again if Resume fails.
 */
internal class PausedBindSupervisor(private val bind: () -> PausedStatusHandle) {
    private var listener: PausedStatusHandle? = null
    private var resuming = false
    var failedBinds = 0
        private set

    val bound: Boolean @Synchronized get() = listener != null

    @Synchronized fun tick(paused: Boolean) {
        if (!paused || resuming) { closeLocked(); return }
        if (listener?.alive == false) closeLocked()
        if (listener != null) return
        listener = runCatching { bind() }.getOrNull()
        if (listener == null) ++failedBinds else failedBinds = 0
    }

    fun releaseForResume(resume: () -> Boolean): Boolean {
        synchronized(this) { resuming = true; closeLocked() }
        try {
            return resume()
        } finally {
            synchronized(this) { resuming = false }
        }
    }

    private fun closeLocked() {
        listener?.let { runCatching { it.close() } }
        listener = null
    }
}

/**
 * Process-wide owner: bound while Pause is saved and the app process lives,
 * not tied to a screen or ViewModel (swiping the activity away while paused
 * keeps the answer as long as the process lives).
 */
object PausedStatusEndpoint {
    private val supervisor = PausedBindSupervisor { PausedStatusListener() }
    @Volatile private var started = false

    /** Idempotent; any component of the process may call it first. */
    fun start(context: Context) {
        if (started) return
        synchronized(this) {
            if (started) return
            started = true
        }
        val store = StreamingIntentStore.get(context.applicationContext)
        thread(name = "OkuFlow-USB-paused-supervisor", isDaemon = true) {
            var warned = false
            while (true) {
                supervisor.tick(store.paused.value)
                if (supervisor.failedBinds >= 20 && !warned) {
                    warned = true
                    Log.w("OkuFlow-USB", "Paused status listener still cannot bind; retrying")
                }
                if (supervisor.bound) warned = false
                Thread.sleep(PAUSED_SUPERVISE_MS)
            }
        }
    }

    /** Releases the socket name before [resume] lets the camera service start. */
    fun releaseForResume(resume: () -> Boolean): Boolean = supervisor.releaseForResume(resume)
}
