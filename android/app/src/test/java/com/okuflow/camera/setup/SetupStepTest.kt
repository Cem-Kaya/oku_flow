package com.okuflow.camera.setup

import com.okuflow.camera.link.HostLinkState
import com.okuflow.camera.link.HostProblem
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.assertNull
import org.junit.Test

class SetupStepTest {

    private val ready = DeviceSetupSnapshot(
        developerOptionsEnabled = true,
        usbDebuggingEnabled = true,
        cameraPermissionGranted = true,
        usbConnectedToComputer = true,
    )

    private fun step(
        snapshot: DeviceSetupSnapshot,
        link: HostLinkState = HostLinkState.Disconnected,
        problem: HostProblem? = null,
    ) = resolveSetupStep(snapshot, link, problem)

    @Test
    fun freshPhoneAsksForDeveloperOptionsOnly() {
        val fresh = DeviceSetupSnapshot(
            developerOptionsEnabled = false,
            usbDebuggingEnabled = false,
            cameraPermissionGranted = false,
            usbConnectedToComputer = false,
        )
        assertEquals(SetupStep.EnableDeveloperOptions, step(fresh))
    }

    @Test
    fun developerOptionsOnButUsbDebuggingOffAsksForUsbDebugging() {
        assertEquals(
            SetupStep.EnableUsbDebugging,
            step(ready.copy(usbDebuggingEnabled = false)),
        )
    }

    @Test
    fun usbDebuggingOnImpliesDeveloperOptionsEvenWhenOemReportsOff() {
        assertEquals(
            SetupStep.OpenOkuFlow,
            step(ready.copy(developerOptionsEnabled = false)),
        )
    }

    @Test
    fun missingCameraPermissionComesAfterBothSwitches() {
        assertEquals(
            SetupStep.AllowCamera,
            step(ready.copy(cameraPermissionGranted = false, usbConnectedToComputer = false)),
        )
    }

    @Test
    fun unpluggedPhoneAsksForCable() {
        assertEquals(SetupStep.ConnectUsb, step(ready.copy(usbConnectedToComputer = false)))
    }

    @Test
    fun pluggedPhoneWithoutSessionAsksToOpenOkuFlow() {
        assertEquals(SetupStep.OpenOkuFlow, step(ready))
    }

    @Test
    fun activeSessionShowsStreaming() {
        assertEquals(SetupStep.Streaming, step(ready, HostLinkState.Streaming))
    }

    @Test
    fun streamingNeverHidesAMissingPrerequisite() {
        assertEquals(
            SetupStep.AllowCamera,
            step(ready.copy(cameraPermissionGranted = false), HostLinkState.Streaming),
        )
    }

    @Test
    fun streamingTrustsTheLiveSessionOverACoarseUsbReading() {
        // The battery-plug fallback can misreport a computer as a charger;
        // a live adb session proves the cable carries data.
        assertEquals(
            SetupStep.Streaming,
            step(ready.copy(usbConnectedToComputer = false), HostLinkState.Streaming),
        )
    }

    @Test
    fun openSessionShowsConnectedEvenWithACoarseUsbReading() {
        assertEquals(SetupStep.HostConnected, step(ready, HostLinkState.Connected))
        assertEquals(SetupStep.HostConnected, step(ready.copy(usbConnectedToComputer = false), HostLinkState.Connected))
        // Android 17 hides both switches; a live session proves debugging works.
        val redacted = ready.copy(developerOptionsEnabled = null, usbDebuggingEnabled = null)
        assertEquals(SetupStep.HostConnected, step(redacted, HostLinkState.Connected))
    }

    @Test
    fun problemOutranksAnOpenSessionAndPersistsAfterTheHostLeaves() {
        assertEquals(SetupStep.Problem, step(ready, HostLinkState.Connected, HostProblem.CameraBusy))
        assertEquals(SetupStep.Problem, step(ready, HostLinkState.Disconnected, HostProblem.CameraBusy))
        assertEquals(SetupStep.Problem,
            step(ready.copy(developerOptionsEnabled = null, usbDebuggingEnabled = null), problem = HostProblem.TooHot))
    }

    @Test
    fun problemNeverHidesStreamingACableOrAPrerequisite() {
        assertEquals(SetupStep.Streaming, step(ready, HostLinkState.Streaming, HostProblem.CameraFailed))
        assertEquals(SetupStep.ConnectUsb, step(ready.copy(usbConnectedToComputer = false), problem = HostProblem.CameraBusy))
        assertEquals(SetupStep.AllowCamera,
            step(ready.copy(cameraPermissionGranted = false), HostLinkState.Connected, HostProblem.CameraBlocked))
        assertEquals(SetupStep.EnableUsbDebugging,
            step(ready.copy(usbDebuggingEnabled = false), problem = HostProblem.ServiceFailed))
    }

    @Test
    fun waitingAndProblemStatesAreUnnumbered() {
        assertNull(SetupStep.HostConnected.actionNumber)
        assertNull(SetupStep.Problem.actionNumber)
    }

    @Test
    fun actionStepsAreNumberedOneToThree() {
        assertEquals(1, SetupStep.EnableDeveloperOptions.actionNumber)
        assertEquals(2, SetupStep.EnableUsbDebugging.actionNumber)
        assertEquals(3, SetupStep.AllowCamera.actionNumber)
        assertEquals(SetupStep.ACTION_STEP_COUNT, SetupStep.AllowCamera.actionNumber)
    }

    @Test
    fun android17RedactedZerosAreUnknownButOlderDisabledSettingsStayFalse() {
        assertNull(debuggingSettingEnabled(0, settingsRedacted = true))
        assertEquals(false, debuggingSettingEnabled(0, settingsRedacted = false))
        assertEquals(true, debuggingSettingEnabled(1, settingsRedacted = true))
        assertNull(debuggingSettingEnabled(-1, settingsRedacted = false))
    }

    @Test
    fun activeUsbAdbSkipsRedactedSettingsWithoutBypassingCameraPermission() {
        val connected = ready.copy(developerOptionsEnabled = null, usbDebuggingEnabled = null, usbAdbActive = true)
        assertEquals(SetupStep.OpenOkuFlow, step(connected))
        assertEquals(SetupStep.AllowCamera, step(connected.copy(cameraPermissionGranted = false)))
        assertTrue(connected.canListenForHost)
        // Also handles OEM backports that redact zero on older Android releases.
        assertEquals(SetupStep.OpenOkuFlow, step(connected.copy(developerOptionsEnabled = false, usbDebuggingEnabled = false)))
    }

    @Test
    fun unknownSettingsPermitHandshakeAndShowHelpWithoutClaimingSwitchesAreOff() {
        val unknown = ready.copy(developerOptionsEnabled = null, usbDebuggingEnabled = null)
        assertEquals(SetupStep.CheckUsbSetup, step(unknown))
        assertEquals(SetupStep.Streaming, step(unknown, HostLinkState.Streaming))
        assertTrue(unknown.canListenForHost)
        assertFalse(unknown.copy(cameraPermissionGranted = false).canListenForHost)
        assertFalse(unknown.copy(usbConnectedToComputer = false).canListenForHost)
        assertFalse(ready.copy(usbDebuggingEnabled = false).canListenForHost)
        assertEquals(SetupStep.ConnectUsb, step(unknown.copy(usbConnectedToComputer = false)))
    }
}
