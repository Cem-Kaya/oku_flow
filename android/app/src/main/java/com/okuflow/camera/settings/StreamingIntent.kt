package com.okuflow.camera.settings

/** Persistence boundary is separate so reopen/retry intent can be tested without Android. */
interface StreamingPausePersistence {
    fun readPaused(): Boolean
    fun writePaused(paused: Boolean): Boolean
}

/** Only an explicit user Resume clears Pause; lifecycle/USB/host events cannot change it. */
class StreamingIntent(private val persistence: StreamingPausePersistence) {
    @Volatile var paused: Boolean = persistence.readPaused()
        private set

    fun canStart(prerequisitesReady: Boolean): Boolean = prerequisitesReady && !paused

    @Synchronized fun pause(): Boolean {
        // Stop this process even if storage fails; the UI reports inability to save the next launch's intent.
        paused = true
        return persistence.writePaused(true)
    }

    @Synchronized fun resume(): Boolean {
        // A failed write must not resume a stream while the persisted intent still says Pause.
        if (!persistence.writePaused(false)) return false
        paused = false
        return true
    }
}
