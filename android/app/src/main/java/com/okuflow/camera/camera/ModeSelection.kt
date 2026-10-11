package com.okuflow.camera.camera

import com.okuflow.camera.settings.CameraSettings

/** An actual camera/encoder size and rate, before host or thermal restrictions. */
data class StreamMode(val width: Int, val height: Int, val fps: Int, val bitrate: Int) {
    val pixels: Long get() = width.toLong() * height
    val key: String get() = "${width}x${height}@$fps"
}

/** Keep the everyday picker useful while retaining the complete local catalog for More options. */
fun everydayModes(modes: List<StreamMode>): List<StreamMode> =
    modes.filter { minOf(it.width, it.height) >= 720 && it.fps >= 24 }

/** The same bounds govern the persisted selection and runtime fallback. */
fun StreamMode.fits(resolution: Int, fpsLimit: Int, maxWidth: Int = 0, maxHeight: Int = 0): Boolean =
    (resolution == 0 || minOf(width, height) <= resolution) && fps <= fpsLimit &&
        (maxWidth == 0 || width <= maxWidth) && (maxHeight == 0 || height <= maxHeight)

fun chooseMode(modes: List<StreamMode>, resolution: Int, fps: Int, maxWidth: Int = 0, maxHeight: Int = 0): StreamMode {
    val allowed = modes.filter { it.fits(resolution, fps, maxWidth, maxHeight) }
    require(allowed.isNotEmpty()) { "No compatible camera/encoder mode" }
    return allowed.maxWith(compareBy<StreamMode> { it.pixels }.thenBy { it.fps })
}

/** Migrate legacy ceilings and reconcile a changed camera/codec without changing image controls. */
fun CameraSettings.withAvailableModes(modes: List<StreamMode>): CameraSettings {
    if (modes.isEmpty() || (!hasExplicitMode && resolution == 0 && fps == 60)) return this
    val selected = runCatching {
        chooseMode(modes, if (hasExplicitMode) 0 else resolution, fps, modeWidth, modeHeight)
    }.getOrNull()
    return if (selected == null) copy(resolution = 0, modeWidth = 0, modeHeight = 0, fps = 60)
    else copy(resolution = 0, modeWidth = selected.width, modeHeight = selected.height, fps = selected.fps)
}
