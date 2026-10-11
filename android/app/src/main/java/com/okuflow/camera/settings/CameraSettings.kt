package com.okuflow.camera.settings

import android.content.Context
import androidx.core.content.edit
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Preferred codec for new installations and Reset. HEVC is only a preference:
 * capture falls back to AVC when the host or a local encoder lacks HEVC.
 */
const val DEFAULT_CODEC = "hevc"

/** A missing stored codec takes the default; an existing explicit choice, including "avc", is kept. */
internal fun storedCodec(saved: String?): String = saved ?: DEFAULT_CODEC

/** Installation-local preferences plus session-only torch state. Zero means automatic. */
data class CameraSettings(
    val cameraId: String = "",
    val resolution: Int = 0,
    val fps: Int = 60,
    val modeWidth: Int = 0,
    val modeHeight: Int = 0,
    val bitrateMbps: Int = 0,
    val codec: String = DEFAULT_CODEC,
    val stabilization: String = "optical",
    val torch: Boolean = false,
    val exposure: Int = 0,
    val autofocus: Boolean = true,
) {
    val hasExplicitMode: Boolean get() = modeWidth > 0 && modeHeight > 0

    /** Light needs a new explicit host action after each connection. */
    fun forNewHostSession() = copy(torch = false)

    fun sanitized() = copy(
        resolution = resolution.takeIf { it in listOf(0, 720, 1080, 1440, 2160) } ?: 0,
        fps = fps.takeIf { it in 1..60 } ?: 60,
        modeWidth = modeWidth.takeIf { it in 2..16384 && modeHeight in 2..16384 } ?: 0,
        modeHeight = modeHeight.takeIf { it in 2..16384 && modeWidth in 2..16384 } ?: 0,
        bitrateMbps = bitrateMbps.takeIf { it in listOf(0, 10, 20, 40, 80) } ?: 0,
        // Unknown/corrupt values stay on the conservative, universally decodable AVC.
        codec = if (codec == "hevc") "hevc" else "avc",
        stabilization = stabilization.takeIf { it in listOf("off", "optical", "electronic") } ?: "optical",
        exposure = exposure.coerceIn(-12, 12),
    )
}

/** Shared by UI/service. Preferences persist; torch is published only for the live session. */
class CameraSettingsStore(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences("camera", Context.MODE_PRIVATE)
    private val mutable = MutableStateFlow(read())
    val state = mutable.asStateFlow()

    @Synchronized
    fun update(settings: CameraSettings) {
        val value = settings.sanitized()
        preferences.edit {
            putString("camera", value.cameraId)
            putInt("resolution", value.resolution)
            putInt("fps", value.fps)
            putInt("modeWidth", value.modeWidth)
            putInt("modeHeight", value.modeHeight)
            putInt("bitrate", value.bitrateMbps)
            putString("codec", value.codec)
            putString("stabilization", value.stabilization)
            remove("torch") // Remove legacy light-on preferences; never restore the light.
            putInt("exposure", value.exposure)
            putBoolean("autofocus", value.autofocus)
        }
        mutable.value = value
    }

    private fun read() = CameraSettings(
        cameraId = preferences.getString("camera", "") ?: "",
        resolution = preferences.getInt("resolution", 0),
        fps = preferences.getInt("fps", 60),
        modeWidth = preferences.getInt("modeWidth", 0),
        modeHeight = preferences.getInt("modeHeight", 0),
        bitrateMbps = preferences.getInt("bitrate", 0),
        codec = storedCodec(preferences.getString("codec", null)),
        stabilization = preferences.getString("stabilization", "optical") ?: "optical",
        torch = false,
        exposure = preferences.getInt("exposure", 0),
        autofocus = preferences.getBoolean("autofocus", true),
    ).sanitized()

    companion object {
        @Volatile private var instance: CameraSettingsStore? = null
        fun get(context: Context): CameraSettingsStore = instance ?: synchronized(this) {
            instance ?: CameraSettingsStore(context).also { instance = it }
        }
    }
}
