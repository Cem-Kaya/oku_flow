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

/**
 * New camera foreground services require a resumed activity; a refusal waits
 * for a fresh resume or a new launch by the computer (which may come while
 * the activity is already resumed and so never produces a resume).
 *
 * Main thread only, so the plain fields need no cross-thread visibility:
 * every caller runs there (activity lifecycle and `onNewIntent`, the Compose
 * effects on the main dispatcher that call resume/pause and the periodic
 * start, and the service's `onCreate`). A caller on another thread must
 * post to the main thread first.
 */
internal class VisibleServiceStartGate {
    private var resumed = false
    private var refused = false
    val canStart: Boolean get() = resumed && !refused

    fun onResumed() { resumed = true; refused = false }
    fun onPaused() { resumed = false }
    fun onRefused() { refused = true }
    fun onHostLaunch() { refused = false }
}
