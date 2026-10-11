package com.okuflow.camera.setup

import android.app.Application
import android.content.Context
import androidx.core.content.edit
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.okuflow.camera.link.HostProblem
import com.okuflow.camera.link.HostLink
import com.okuflow.camera.camera.CameraStreamService
import com.okuflow.camera.settings.CameraSettings
import com.okuflow.camera.settings.CameraSettingsStore
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn

class SetupViewModel(application: Application) : AndroidViewModel(application) {
    private val setupPreferences = application.getSharedPreferences("setup", Context.MODE_PRIVATE)
    private val monitor = DeviceSetupMonitor(application).also { it.start() }
    private val settingsStore = CameraSettingsStore.get(application)
    val settings = settingsStore.state
    val diagnostics = HostLink.details.asStateFlow()
    val problem = HostLink.problem

    fun updateSettings(settings: CameraSettings) {
        if (settings != settingsStore.state.value && HostLink.problem.value in listOf(
                HostProblem.NoCompatibleMode, HostProblem.CameraBusy, HostProblem.CameraFailed)) HostLink.report(null)
        settingsStore.update(settings)
    }

    /** Returning from privacy settings or another camera app is a new user-authorized attempt. */
    fun onResumed() {
        if (HostLink.problem.value == HostProblem.CameraBlocked) HostLink.report(null)
    }

    /**
     * Called by the resumed activity, also periodically, so a failed or stopped
     * listener comes back in any phone/cable/authorization order. Background
     * service starts never bypass Android's camera gate.
     */
    fun startServiceIfReady() {
        val snapshot = monitor.snapshot.value
        if (snapshot.canListenForHost && !CameraStreamService.isRunning)
            CameraStreamService.startVisible(getApplication())
    }

    val step: StateFlow<SetupStep> =
        combine(monitor.snapshot, HostLink.state, HostLink.problem, ::resolveSetupStep)
            .stateIn(
                viewModelScope,
                SharingStarted.Eagerly,
                resolveSetupStep(monitor.snapshot.value, HostLink.state.value, HostLink.problem.value),
            )

    private val mutableCameraPermanentlyDenied = MutableStateFlow(
        setupPreferences.getBoolean("camera_permanently_denied", false) &&
            !monitor.snapshot.value.cameraPermissionGranted,
    )

    /**
     * True after the user denied camera access with "Don't ask again" (or
     * denied twice): the system dialog no longer appears, so the camera step
     * opens this app's settings page instead.
     */
    val cameraPermanentlyDenied: StateFlow<Boolean> = mutableCameraPermanentlyDenied.asStateFlow()

    fun refresh() {
        HostLink.retryTransientProblem()
        monitor.refresh()
        if (monitor.snapshot.value.cameraPermissionGranted) {
            setCameraPermanentlyDenied(false)
        }
        startServiceIfReady()
    }

    /**
     * [shouldShowRationale] is only meaningful right after a denial: false
     * then means the system will not show the permission dialog again.
     */
    fun onCameraPermissionResult(granted: Boolean, shouldShowRationale: Boolean) {
        setCameraPermanentlyDenied(!granted && !shouldShowRationale)
        if (granted && HostLink.problem.value == HostProblem.CameraBlocked) HostLink.report(null)
        refresh()
    }

    // Android's denial state survives process death. Keep the settings shortcut
    // available on the next launch, then clear it after access is granted.
    private fun setCameraPermanentlyDenied(denied: Boolean) {
        if (mutableCameraPermanentlyDenied.value == denied &&
            setupPreferences.getBoolean("camera_permanently_denied", false) == denied) return
        mutableCameraPermanentlyDenied.value = denied
        setupPreferences.edit { putBoolean("camera_permanently_denied", denied) }
    }

    override fun onCleared() {
        monitor.stop()
    }
}
