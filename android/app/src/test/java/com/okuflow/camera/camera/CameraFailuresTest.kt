package com.okuflow.camera.camera

import com.okuflow.camera.link.HostProblem
import org.junit.Assert.assertEquals
import org.junit.Test

class CameraFailuresTest {

    @Test
    fun deviceErrorsNameTheUserFixableCause() {
        // CameraDevice.StateCallback.ERROR_* values.
        assertEquals(HostProblem.CameraBusy, problemForDeviceError(1))
        assertEquals(HostProblem.CameraBusy, problemForDeviceError(2))
        assertEquals(HostProblem.CameraBlocked, problemForDeviceError(3))
        assertEquals(HostProblem.CameraFailed, problemForDeviceError(4))
        assertEquals(HostProblem.CameraFailed, problemForDeviceError(5))
    }

    @Test
    fun openCameraAccessReasonsMatchDeviceErrors() {
        // CameraAccessException reasons: DISABLED 1, DISCONNECTED 2, ERROR 3, IN_USE 4, MAX_IN_USE 5.
        assertEquals(HostProblem.CameraBlocked, problemForAccessReason(1))
        assertEquals(HostProblem.CameraBusy, problemForAccessReason(2))
        assertEquals(HostProblem.CameraFailed, problemForAccessReason(3))
        assertEquals(HostProblem.CameraBusy, problemForAccessReason(4))
        assertEquals(HostProblem.CameraBusy, problemForAccessReason(5))
    }
}
