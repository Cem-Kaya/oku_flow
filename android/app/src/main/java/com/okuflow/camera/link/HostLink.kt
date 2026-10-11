package com.okuflow.camera.link

import android.os.SystemClock
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Connection state between this phone and the OkuFlow Windows app. */
enum class HostLinkState {
    /** No OkuFlow session over the adb tunnel. */
    Disconnected,

    /** OkuFlow holds a control/video session; the camera is starting, restarting or paused. */
    Connected,

    /** A host is connected and the phone is producing encoded camera frames. */
    Streaming,
}

/**
 * Why the phone could not deliver video, phrased for the user rather than
 * the host. The raw diagnostic still goes to Windows in an ERROR packet.
 */
enum class HostProblem(
    /** Stable ERROR/probe `code` hosts use for backoff; never shown to users. */
    val wireCode: String,
) {
    /** Another app, or a higher-priority camera client, holds the camera. */
    CameraBusy("busy"),

    /** Device policy, the camera privacy switch or a revoked permission blocks access. */
    CameraBlocked("blocked"),

    /** No camera/encoder pair fits the saved mode and the host's limits. */
    NoCompatibleMode("noMode"),

    /** The camera or encoder failed for another reason. */
    CameraFailed("failed"),

    /** Thermal status reached CRITICAL; capture stopped until the phone cools. */
    TooHot("hot"),

    /** The foreground service or its USB socket could not start. */
    ServiceFailed("failed"),
}

/** ERROR code for a control hello rejected because another host holds the session. */
const val SESSION_BUSY_CODE = "sessionBusy"

/** Unbound control sessions are closed after this, so a stalled host cannot hold the single slot. */
const val VIDEO_BIND_DEADLINE_MS = 5_000L

internal fun videoBindExpired(createdAtMs: Long, nowMs: Long, videoBound: Boolean): Boolean =
    !videoBound && nowMs - createdAtMs > VIDEO_BIND_DEADLINE_MS

/** adb shell (2000) and root (0) only; an unreadable peer is refused. */
internal fun peerAllowed(uid: Int?): Boolean = uid == 2000 || uid == 0

/**
 * Process-wide publication point written by CameraStreamService and read by the UI.
 * A problem survives the host disconnecting after its ERROR packet; it is
 * cleared by the next session, the next stream, cooling or unplugging.
 */
internal fun canRetryProblem(problem: HostProblem?, state: HostLinkState, elapsedMs: Long): Boolean =
    state == HostLinkState.Disconnected && elapsedMs >= 30_000 &&
        problem in listOf(HostProblem.CameraBusy, HostProblem.CameraFailed)

object HostLink {
    private val mutableState = MutableStateFlow(HostLinkState.Disconnected)
    private val mutableProblem = MutableStateFlow<HostProblem?>(null)
    @Volatile private var problemAtMs = 0L

    val state: StateFlow<HostLinkState> = mutableState.asStateFlow()
    val problem: StateFlow<HostProblem?> = mutableProblem.asStateFlow()

    /** Actual negotiated mode while connected, for status lines; empty otherwise. */
    val details = MutableStateFlow("")

    internal fun publish(state: HostLinkState) {
        mutableState.value = state
    }

    internal fun report(problem: HostProblem?) {
        problemAtMs = if (problem == null) 0L else SystemClock.elapsedRealtime()
        mutableProblem.value = problem
    }

    /** Retry transient camera failures, retaining their visible warning for at least 30 seconds. */
    internal fun retryTransientProblem() {
        if (canRetryProblem(problem.value, state.value, SystemClock.elapsedRealtime() - problemAtMs)) report(null)
    }
}
