package com.okuflow.camera.settings

import com.okuflow.camera.link.HostLinkState
import com.okuflow.camera.setup.DeviceSetupSnapshot
import com.okuflow.camera.setup.SetupStep
import com.okuflow.camera.setup.resolveSetupStep
import org.junit.Assert.*
import org.junit.Test

class StreamingIntentTest {
    private class SavedPause : StreamingPausePersistence {
        var value = false
        var failWrites = false
        override fun readPaused() = value
        override fun writePaused(paused: Boolean): Boolean {
            if (failWrites) return false
            value = paused
            return true
        }
    }
    private val ready = DeviceSetupSnapshot(true, true, true, true)

    @Test fun freshInstallStillAutoConnectsOnlyWithPrerequisites() {
        val intent = StreamingIntent(SavedPause())
        assertTrue(intent.canStart(ready.canListenForHost))
        assertFalse(intent.canStart(ready.copy(cameraPermissionGranted = false).canListenForHost))
        assertFalse(intent.canStart(ready.copy(usbConnectedToComputer = false).canListenForHost))
    }
    @Test fun savedPauseSurvivesAppRelaunchAndRepeatedHostUsbPermissionRetries() {
        val disk = SavedPause()
        val original = StreamingIntent(disk)
        assertTrue(original.pause())
        val reopened = StreamingIntent(disk)
        repeat(20) {
            assertFalse(reopened.canStart(false))
            assertFalse(reopened.canStart(ready.canListenForHost))
            assertEquals(SetupStep.Paused, resolveSetupStep(ready, HostLinkState.Streaming, paused = reopened.paused))
        }
        assertTrue(reopened.resume())
        assertTrue(reopened.canStart(true))
        assertFalse(StreamingIntent(disk).paused)
    }
    @Test fun explicitResumeDoesNotBypassCameraOrCableRequirements() {
        val intent = StreamingIntent(SavedPause())
        intent.pause()
        assertTrue(intent.resume())
        assertFalse(intent.canStart(ready.copy(cameraPermissionGranted = false).canListenForHost))
        assertFalse(intent.canStart(ready.copy(usbConnectedToComputer = false).canListenForHost))
    }
    @Test fun failedPauseWriteStillBlocksThisProcessAndFailedResumeStaysPaused() {
        val disk = SavedPause().apply { failWrites = true }
        val intent = StreamingIntent(disk)
        assertFalse(intent.pause())
        assertFalse(intent.canStart(true))
        assertFalse(intent.resume())
        assertTrue(intent.paused)
        disk.failWrites = false
        assertTrue(intent.pause())
        assertTrue(StreamingIntent(disk).paused)
    }
    @Test fun pausedDisplayDoesNotAutomaticallyRequestRevokedPermissionOrClaimSetupComplete() {
        assertEquals(SetupStep.Paused, resolveSetupStep(ready.copy(cameraPermissionGranted = false),
            HostLinkState.Disconnected, paused = true))
        assertNull(SetupStep.Paused.actionNumber)
        assertEquals(SetupStep.AllowCamera, resolveSetupStep(ready.copy(cameraPermissionGranted = false),
            HostLinkState.Disconnected, paused = false))
    }
}
