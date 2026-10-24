package com.okuflow.camera.camera

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.util.Range
import com.okuflow.camera.settings.CameraSettings
import com.okuflow.camera.link.HostLimits
import org.json.JSONArray
import org.json.JSONObject

data class CameraChoice(val id: String, val facing: Int, val orientation: Int,
    val deviceId: String = id, val physicalId: String? = null, val logical: Boolean = false,
    val equivalentMm: Float? = null, val role: String = "back")
data class EncoderChoice(val name: String, val mime: String, val capabilities: MediaCodecInfo.VideoCapabilities)
data class CameraConfiguration(
    val camera: CameraChoice,
    val characteristics: CameraCharacteristics,
    val encoder: EncoderChoice,
    val mode: StreamMode,
    val fpsRange: Range<Int>,
)

/** Intersects regular Camera2 recording outputs with hardware encoder size/rate limits. */
class CameraCatalog(context: Context) {
    val manager = context.getSystemService(CameraManager::class.java)

    fun cameras(): List<CameraChoice> {
        val publicIds = manager.cameraIdList.toSet()
        fun choice(id: String, device: String = id, physical: String? = null): CameraChoice {
            val chars = manager.getCameraCharacteristics(physical ?: id)
            val facing = chars[CameraCharacteristics.LENS_FACING] ?: -1
            val sensor = chars[CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE]
            val equivalent = equivalentFocalLength(chars[CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS]?.firstOrNull(),
                sensor?.width, sensor?.height)
            val logical = chars[CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES]?.contains(
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA) == true
            return CameraChoice(id, facing, chars[CameraCharacteristics.SENSOR_ORIENTATION] ?: 0,
                device, physical, logical, equivalent, if (logical) "auto" else lensRole(facing == CameraCharacteristics.LENS_FACING_FRONT, equivalent))
        }
        val choices = publicIds.mapNotNull { runCatching { choice(it) }.getOrNull() }.toMutableList()
        for (parent in choices.toList().filter { it.logical }) {
            val chars = manager.getCameraCharacteristics(parent.deviceId)
            for (physical in chars.physicalCameraIds.sorted()) {
                if (physical in publicIds) continue // Direct-open identity is preferable when exposed.
                runCatching {
                    val id = "physical/${android.net.Uri.encode(parent.id)}/${android.net.Uri.encode(physical)}"
                    choice(id, parent.deviceId, physical)
                }.getOrNull()?.let(choices::add)
            }
        }
        val automaticId = automaticCamera(choices)?.id
        return choices.sortedWith(compareBy<CameraChoice> { it.id != automaticId }
            .thenBy { it.facing != CameraCharacteristics.LENS_FACING_BACK }
            .thenBy { it.equivalentMm ?: Float.MAX_VALUE }.thenBy { it.id }).take(31)
    }

    private fun encoders(mime: String): List<EncoderChoice> =
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter {
            it.isEncoder && it.isHardwareAccelerated && it.supportedTypes.any { type -> type.equals(mime, true) }
        }.mapNotNull { info ->
            runCatching {
                val caps = info.getCapabilitiesForType(mime)
                if (MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface !in caps.colorFormats) null
                else caps.videoCapabilities?.let { EncoderChoice(info.name, mime, it) }
            }.getOrNull()
        }

    /** Local discovery shared by the picker and capture; no PC, network or fixed size presets. */
    fun modes(settings: CameraSettings): List<StreamMode> =
        recordingConfigurations(settings, listOf(settings.codec, "avc").distinct()).map { it.mode }
            .distinctBy { it.key }.sortedWith(compareByDescending<StreamMode> { it.pixels }.thenByDescending { it.fps })

    /** Keep the exact requested pair first; then try only locally supported, smaller/slower modes. */
    fun configurations(settings: CameraSettings, hostCodecs: Set<String>, thermalFps: Int,
        hostLimits: HostLimits): List<CameraConfiguration> =
        recordingConfigurations(settings, listOf(settings.codec, "avc").distinct().filter { it in hostCodecs })
            .filter { config ->
                val mode = config.mode
                mode.fits(if (settings.hasExplicitMode) 0 else settings.resolution, minOf(settings.fps, thermalFps),
                    settings.modeWidth, settings.modeHeight) &&
                    mode.width <= hostLimits.width && mode.height <= hostLimits.height &&
                    mode.pixels * mode.fps <= hostLimits.pixelsPerSecond
            }.sortedWith(compareByDescending<CameraConfiguration> { it.mode.pixels }.thenByDescending { it.mode.fps })
            .distinctBy { listOf(it.encoder.mime, it.mode.width, it.mode.height, it.mode.fps) }

    private fun recordingConfigurations(settings: CameraSettings, codecs: List<String>): List<CameraConfiguration> {
        val choices = cameras()
        val camera = if (settings.cameraId.isEmpty() || settings.cameraId == "auto") automaticCamera(choices)
            else choices.firstOrNull { it.id == settings.cameraId }
            ?: error("No camera available")
        requireNotNull(camera) { "No camera available" }
        val chars = manager.getCameraCharacteristics(camera.physicalId ?: camera.deviceId)
        val map = chars[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP] ?: error("No recording outputs")
        val ranges = chars[CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES]?.toList().orEmpty()
        return codecs.flatMap { codec ->
            encoders(if (codec == "hevc") MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC)
                .flatMap { encoder ->
                    map.getOutputSizes(android.media.MediaCodec::class.java).orEmpty().flatMap { size ->
                        val duration = map.getOutputMinFrameDuration(android.media.MediaCodec::class.java, size)
                        ranges.map { it.upper }.distinct().filter { fps ->
                            fps in 1..60 &&
                                (duration == 0L || duration <= 1_000_000_000L / fps + 1000) &&
                                runCatching { encoder.capabilities.areSizeAndRateSupported(
                                    size.width, size.height, fps.toDouble()) }.getOrDefault(false)
                        }.map { fps ->
                            val automatic = (size.width.toLong() * size.height * fps * 0.14).toLong()
                                .coerceIn(4_000_000L, 120_000_000L).toInt()
                            val rate = if (settings.bitrateMbps == 0) automatic else settings.bitrateMbps * 1_000_000
                            val mode = StreamMode(size.width, size.height, fps, rate.coerceIn(
                                encoder.capabilities.bitrateRange.lower, encoder.capabilities.bitrateRange.upper))
                            val range = ranges.filter { it.upper == fps }
                                .minWith(compareBy<Range<Int>> { it.upper - it.lower }.thenBy { it.upper })
                            CameraConfiguration(camera, chars, encoder, mode, range)
                        }
                    }
                }
        }
    }

    fun capabilities(): JSONObject = JSONObject().put("cameras", JSONArray().also { array ->
        cameras().forEach { camera ->
            val chars = manager.getCameraCharacteristics(camera.physicalId ?: camera.deviceId)
            // A fixed physical output bypasses logical-camera lens switching.
            val zoom = if (camera.physicalId == null) chars[CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE] else null
            array.put(JSONObject().put("id", camera.id).put("facing", camera.facing)
                .put("orientation", camera.orientation).put("zoomMin", zoom?.lower ?: 1f)
                .put("zoomMax", zoom?.upper ?: 1f).put("torch", chars[CameraCharacteristics.FLASH_INFO_AVAILABLE] == true)
                .put("role", camera.role).put("logical", camera.logical)
                .put("equivalentFocalMm", camera.equivalentMm)
                .put("timestampSource", chars[CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE] ?: 0))
        }
    }).put("automaticCameraId", "auto").put("automaticCamera", automaticCamera(cameras())?.id ?: "")
        .put("codecs", JSONArray().also { array ->
        if (encoders(MediaFormat.MIMETYPE_VIDEO_AVC).isNotEmpty()) array.put("avc")
        if (encoders(MediaFormat.MIMETYPE_VIDEO_HEVC).isNotEmpty()) array.put("hevc")
    })
}
