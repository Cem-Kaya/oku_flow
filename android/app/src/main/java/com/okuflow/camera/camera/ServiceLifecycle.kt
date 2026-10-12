package com.okuflow.camera.camera

/** Owns a listener only after registration succeeds, including partial service startup. */
internal class ListenerRegistration(
    private val registerListener: () -> Unit,
    private val unregisterListener: () -> Unit,
) : AutoCloseable {
    private var registered = false

    fun register() {
        if (registered) return
        registerListener()
        registered = true
    }

    override fun close() {
        if (!registered) return
        registered = false
        unregisterListener()
    }
}

/** New camera foreground services require a resumed activity; a refusal waits for a fresh resume. */
internal class VisibleServiceStartGate {
    private var resumed = false
    private var refused = false
    val canStart: Boolean get() = resumed && !refused

    fun onResumed() { resumed = true; refused = false }
    fun onPaused() { resumed = false }
    fun onRefused() { refused = true }
}
