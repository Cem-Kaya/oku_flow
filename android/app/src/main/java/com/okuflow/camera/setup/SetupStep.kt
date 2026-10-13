package com.okuflow.camera.setup

import com.okuflow.camera.link.HostLinkState
import com.okuflow.camera.link.HostProblem

/**
 * Everything the setup screen needs to know about the phone, read from
 * system settings and sticky broadcasts by [DeviceSetupMonitor].
 */
data class DeviceSetupSnapshot(
    val developerOptionsEnabled: Boolean?,
    val usbDebuggingEnabled: Boolean?,
    val cameraPermissionGranted: Boolean,
    val usbConnectedToComputer: Boolean,
    /** Current system USB broadcast advertises the adb function; not cached across unplug. */
    val usbAdbActive: Boolean = false,
) {
    val canListenForHost: Boolean
        get() = cameraPermissionGranted && usbConnectedToComputer &&
            (usbAdbActive || usbDebuggingEnabled != false)
}

/** Debounce USB broadcasts before retiring a host session; the listener survives cable removal. */
const val USB_DISCONNECT_GRACE_MS = 2_000L

/**
 * A listener started from the visible app remains available across cable reconnection.
 * Only lost camera permission or explicitly disabled debugging revokes that service.
 * Android 17's unknown setting is not evidence that debugging was disabled.
 */
fun listenerMustStop(snapshot: DeviceSetupSnapshot): Boolean =
    !snapshot.cameraPermissionGranted || (!snapshot.usbAdbActive && snapshot.usbDebuggingEnabled == false)

/** Android 17 redacts both settings to zero. Unknown must not mean disabled. */
internal fun debuggingSettingEnabled(value: Int, settingsRedacted: Boolean): Boolean? = when {
    value == 1 -> true
    value < 0 || settingsRedacted -> null
    else -> false
}

/**
 * The single thing the user is asked to do right now. The screen shows
 * exactly one step at a time; steps are resolved in this order.
 */
enum class SetupStep(
    /** 1-based position among the steps the user acts on, or null for waiting states. */
    val actionNumber: Int?,
) {
    EnableDeveloperOptions(1),
    EnableUsbDebugging(2),
    AllowCamera(3),
    ConnectUsb(null),
    CheckUsbSetup(null),
    OpenOkuFlow(null),
    Paused(null),

    /** OkuFlow holds a session; the camera is starting or paused. */
    HostConnected(null),

    /** Capture failed or stopped; the screen names the cause and what to do. */
    Problem(null),
    Streaming(null);

    companion object {
        /** Number of steps that need a user action ("Step n of [ACTION_STEP_COUNT]"). */
        const val ACTION_STEP_COUNT = 3
    }
}

/** Public system screen for a setup panel's primary settings action. */
enum class SetupSettingsTarget { AboutPhone, DeveloperOptions }

/**
 * Unknown/redacted debugging state is not proof that Developer options exist.
 * Opening that hidden activity can succeed but immediately finish with an OEM
 * toast, so exception-based fallback cannot fix it. Start at About phone;
 * connection help also exposes a separate explicit Developer options shortcut.
 */
fun primarySetupSettingsTarget(step: SetupStep): SetupSettingsTarget? = when (step) {
    SetupStep.EnableDeveloperOptions, SetupStep.CheckUsbSetup -> SetupSettingsTarget.AboutPhone
    SetupStep.EnableUsbDebugging -> SetupSettingsTarget.DeveloperOptions
    else -> null
}

/**
 * Pure resolution of the current step, kept free of Android types so it is
 * covered by JVM unit tests.
 *
 * USB debugging cannot be on while developer options are off, but some OEM
 * builds report `development_settings_enabled` inconsistently. An enabled
 * `adb_enabled` therefore also counts as developer options being on.
 *
 * A live session proves the cable carries adb, so it outranks the coarse
 * USB reading. A [problem] outranks a still-open session (the host usually
 * disconnects after the error) but never a missing cable or prerequisite.
 */
fun resolveSetupStep(
    snapshot: DeviceSetupSnapshot,
    link: HostLinkState,
    problem: HostProblem? = null,
    paused: Boolean = false,
): SetupStep {
    val debuggingOn = snapshot.usbAdbActive || snapshot.usbDebuggingEnabled == true
    return when {
        paused -> SetupStep.Paused
        !debuggingOn && snapshot.developerOptionsEnabled == false -> SetupStep.EnableDeveloperOptions
        !debuggingOn && snapshot.usbDebuggingEnabled == false -> SetupStep.EnableUsbDebugging
        !snapshot.cameraPermissionGranted -> SetupStep.AllowCamera
        link == HostLinkState.Streaming -> SetupStep.Streaming
        link == HostLinkState.Connected -> if (problem != null) SetupStep.Problem else SetupStep.HostConnected
        !snapshot.usbConnectedToComputer -> SetupStep.ConnectUsb
        problem != null -> SetupStep.Problem
        !debuggingOn -> SetupStep.CheckUsbSetup
        else -> SetupStep.OpenOkuFlow
    }
}
