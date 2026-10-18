package com.okuflow.camera.camera

/** Main-thread owner of host intent. Socket/engine callbacks must post immutable session IDs. */
internal class StreamSessionOwner(
    private val isConnected: (String) -> Boolean,
    private val stopEngine: (String) -> Unit,
) : AutoCloseable {
    private val owner = Thread.currentThread()
    private var closed = false
    var sessionId: String? = null
        private set
    private var requested = false

    private fun checkOwner() = check(Thread.currentThread() === owner) { "Stream lifecycle must use its serial owner" }

    fun bind(id: String): Boolean {
        checkOwner()
        if (closed || !isConnected(id)) return false
        if (sessionId != id) {
            sessionId?.let(stopEngine)
            requested = false
            sessionId = id
        }
        return true
    }

    fun accepts(id: String): Boolean {
        checkOwner()
        return !closed && sessionId == id && isConnected(id)
    }

    fun streaming(id: String): Boolean = accepts(id) && requested

    fun start(id: String): Boolean {
        if (!accepts(id)) return false
        requested = true
        return true
    }

    fun stop(id: String): Boolean {
        checkOwner()
        if (closed || sessionId != id) return false
        requested = false
        stopEngine(id)
        return true
    }

    fun retire(id: String): Boolean {
        if (!stop(id)) return false
        sessionId = null
        return true
    }

    /** Eligibility and engine admission run in the same serial event, never on socket threads. */
    fun restart(startEngine: (String) -> Unit) {
        checkOwner()
        val id = sessionId ?: return
        if (streaming(id)) startEngine(id)
    }

    override fun close() {
        checkOwner()
        if (closed) return
        sessionId?.let { stop(it) }
        sessionId = null
        closed = true
    }
}
