package com.okuflow.camera.link

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
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
class PausedStatusListener : AutoCloseable {
    private val server = LocalServerSocket(SOCKET_NAME)
    private val running = AtomicBoolean(true)

    init {
        thread(name = "OkuFlow-USB-paused", isDaemon = true) {
            while (running.get()) {
                val socket = try { server.accept() } catch (_: Exception) { break }
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

/** Process-wide owner: bound while Pause is saved and the app process lives. */
object PausedStatusEndpoint {
    private var listener: PausedStatusListener? = null

    /**
     * Follows the saved Pause. The camera service releases the socket
     * asynchronously after Pause, so binding retries briefly; Resume closes
     * the status listener before the service binds the socket again.
     */
    fun follow(scope: CoroutineScope, paused: StateFlow<Boolean>) = scope.launch {
        paused.collectLatest { isPaused ->
            if (!isPaused) { close(); return@collectLatest }
            repeat(PAUSED_BIND_ATTEMPTS) {
                if (open()) return@collectLatest
                delay(PAUSED_BIND_RETRY_MS)
            }
            Log.w("OkuFlow-USB", "Paused status listener could not bind")
        }
    }

    @Synchronized private fun open(): Boolean {
        if (listener != null) return true
        listener = runCatching { PausedStatusListener() }.getOrNull()
        return listener != null
    }

    @Synchronized fun close() {
        listener?.close()
        listener = null
    }
}
