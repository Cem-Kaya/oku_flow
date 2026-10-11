package com.okuflow.camera.link

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
enum class HostProblem {
    /** Another app, or a higher-priority camera client, holds the camera. */
    CameraBusy,

    /** Device policy, the camera privacy switch or a revoked permission blocks access. */
    CameraBlocked,

    /** No camera/encoder pair fits the saved mode and the host's limits. */
    NoCompatibleMode,

    /** The camera or encoder failed for another reason. */
    CameraFailed,

    /** Thermal status reached CRITICAL; capture stopped until the phone cools. */
    TooHot,

    /** The foreground service or its USB socket could not start. */
    ServiceFailed,
}

/**
 * Process-wide publication point written by CameraStreamService and read by the UI.
 * A problem survives the host disconnecting after its ERROR packet; it is
 * cleared by the next session, the next stream, cooling or unplugging.
 */
object HostLink {
    private val mutableState = MutableStateFlow(HostLinkState.Disconnected)
    private val mutableProblem = MutableStateFlow<HostProblem?>(null)

    val state: StateFlow<HostLinkState> = mutableState.asStateFlow()
    val problem: StateFlow<HostProblem?> = mutableProblem.asStateFlow()

    /** Actual negotiated mode while connected, for status lines; empty otherwise. */
    val details = MutableStateFlow("")

    internal fun publish(state: HostLinkState) {
        mutableState.value = state
    }

    internal fun report(problem: HostProblem?) {
        mutableProblem.value = problem
    }
}
