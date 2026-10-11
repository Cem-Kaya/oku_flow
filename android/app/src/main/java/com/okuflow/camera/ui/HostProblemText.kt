package com.okuflow.camera.ui

import androidx.annotation.StringRes
import com.okuflow.camera.R
import com.okuflow.camera.link.HostProblem

/** Short title shared by the problem panel, settings status and the service notification. */
@StringRes
fun problemTitle(problem: HostProblem): Int = when (problem) {
    HostProblem.CameraBusy -> R.string.problem_busy_title
    HostProblem.CameraBlocked -> R.string.problem_blocked_title
    HostProblem.NoCompatibleMode -> R.string.problem_mode_title
    HostProblem.CameraFailed -> R.string.problem_failed_title
    HostProblem.TooHot -> R.string.problem_hot_title
    HostProblem.ServiceFailed -> R.string.problem_service_title
}

/** What the user can do about it; each names an action that exists. */
@StringRes
fun problemHint(problem: HostProblem): Int = when (problem) {
    HostProblem.CameraBusy -> R.string.problem_busy_hint
    HostProblem.CameraBlocked -> R.string.problem_blocked_hint
    HostProblem.NoCompatibleMode -> R.string.problem_mode_hint
    HostProblem.CameraFailed -> R.string.problem_failed_hint
    HostProblem.TooHot -> R.string.thermal_stopped
    HostProblem.ServiceFailed -> R.string.problem_service_hint
}
