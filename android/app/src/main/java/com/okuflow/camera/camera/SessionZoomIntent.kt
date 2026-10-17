package com.okuflow.camera.camera

/** Camera-thread intent shared by fallback candidates of exactly one start. */
internal class SessionZoomIntent {
    private var session = ""
    private var generation = 0
    private var desired = 1f

    fun begin(sessionId: String, token: Int) {
        session = sessionId
        generation = token
        desired = 1f
    }

    fun value(sessionId: String, token: Int): Float =
        if (session == sessionId && generation == token) desired else 1f

    fun set(sessionId: String, token: Int, ratio: Float, ready: Boolean, apply: () -> Unit): Boolean {
        require(ratio.isFinite() && ratio > 0) { "Invalid zoom ratio" }
        if (session != sessionId || generation != token) return false
        desired = ratio
        // No CameraDevice/request/session exists yet during startup. Keeping
        // intent is successful admission, not a rejected camera mode.
        if (ready) apply()
        return true
    }
}
