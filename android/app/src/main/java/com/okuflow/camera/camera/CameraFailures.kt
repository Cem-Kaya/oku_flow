package com.okuflow.camera.camera

import android.hardware.camera2.CameraAccessException
import android.hardware.camera2.CameraDevice
import com.okuflow.camera.link.HostProblem

/** CameraDevice.StateCallback.onError codes; the constants are inlined, so JVM tests need no Android runtime. */
internal fun problemForDeviceError(error: Int): HostProblem = when (error) {
    CameraDevice.StateCallback.ERROR_CAMERA_IN_USE,
    CameraDevice.StateCallback.ERROR_MAX_CAMERAS_IN_USE -> HostProblem.CameraBusy
    CameraDevice.StateCallback.ERROR_CAMERA_DISABLED -> HostProblem.CameraBlocked
    else -> HostProblem.CameraFailed
}

/** CameraAccessException.reason values thrown synchronously by openCamera. */
internal fun problemForAccessReason(reason: Int): HostProblem = when (reason) {
    CameraAccessException.CAMERA_IN_USE,
    CameraAccessException.MAX_CAMERAS_IN_USE,
    CameraAccessException.CAMERA_DISCONNECTED -> HostProblem.CameraBusy
    CameraAccessException.CAMERA_DISABLED -> HostProblem.CameraBlocked
    else -> HostProblem.CameraFailed
}

internal fun problemFor(exception: Throwable): HostProblem = when (exception) {
    is CameraAccessException -> problemForAccessReason(exception.reason)
    is SecurityException -> HostProblem.CameraBlocked
    else -> HostProblem.CameraFailed
}
