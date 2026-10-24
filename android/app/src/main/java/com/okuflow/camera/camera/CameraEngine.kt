package com.okuflow.camera.camera

import android.annotation.SuppressLint
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.view.Surface
import com.okuflow.camera.link.WireProtocol
import com.okuflow.camera.link.HostLimits
import com.okuflow.camera.link.HostProblem
import com.okuflow.camera.settings.CameraSettings
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicInteger

/** Camera2 -> encoder input Surface. Only compressed output bytes cross the CPU. All work is serialized. */
class CameraEngine(
    private val catalog: CameraCatalog,
    private val onState: (String, JSONObject) -> Unit,
    private val onMetadata: (String, JSONObject) -> Unit,
    private val onFrame: (String, Long, Long, Int, Int, ByteArray) -> Unit,
    private val onError: (String, HostProblem, String) -> Unit,
) : AutoCloseable {
    private val thread = HandlerThread("OkuFlow-Camera").apply { start() }
    private val handler = Handler(thread.looper)
    private val executor = Executor { handler.post(it) }
    private var active: Run? = null
    private var generation = 0
    private val zoom = SessionZoomIntent()
    private var closed = false
    private val startToken = AtomicInteger()

    private class Run(val config: CameraConfiguration, val settings: CameraSettings, val generation: Int, val token: Int,
        val sessionId: String, val plan: CameraStartupPlan, val index: Int, val attempt: CameraStartupPlan.Attempt) {
        var codec: MediaCodec? = null
        var surface: Surface? = null
        var camera: CameraDevice? = null
        var session: CameraCaptureSession? = null
        var sequence = 0L
        var delivered = false
        var startupFailure: ((HostProblem, String) -> Unit)? = null
    }

    fun start(sessionId: String, settings: CameraSettings, hostCodecs: Set<String>, thermalFps: Int, hostLimits: HostLimits,
        startupBudgetMs: Long = CameraStartupPlan.MAX_STARTUP_MS, startedAtMs: Long = SystemClock.elapsedRealtime()) {
        val token = startToken.incrementAndGet()
        handler.post {
            if (closed || token != startToken.get()) return@post
            stopInternal()
            zoom.begin(sessionId, token)
            try {
                val candidates = catalog.configurations(settings, hostCodecs, thermalFps, hostLimits)
                if (closed || token != startToken.get()) return@post
                if (candidates.isEmpty()) {
                    onError(sessionId, HostProblem.NoCompatibleMode, "No compatible camera/encoder mode within the selected limits")
                    return@post
                }
                val plan = CameraStartupPlan(startedAtMs, startupBudgetMs, candidates.size,
                    SystemClock::elapsedRealtime) { !closed && token == startToken.get() }
                begin(candidates, 0, settings, token, sessionId, plan)
            } catch (exception: Exception) {
                stopInternal()
                if (!closed && token == startToken.get())
                    onError(sessionId, problemFor(exception), exception.message ?: "Unable to start camera")
            }
        }
    }

    private fun begin(candidates: List<CameraConfiguration>, index: Int, settings: CameraSettings, token: Int,
        sessionId: String, plan: CameraStartupPlan) {
        if (closed || token != startToken.get()) return
        val attempt = plan.attempt(index)
        if (attempt == null) {
            onError(sessionId, HostProblem.CameraFailed,
                "Camera startup budget exhausted before candidate ${index + 1}; ${plan.remainingMs()} ms remains " +
                    "(at least ${CameraStartupPlan.MIN_CANDIDATE_MS} ms required per mode)")
            return
        }
        val run = Run(candidates[index], settings, ++generation, token, sessionId, plan, index, attempt)
        run.startupFailure = { problem, message ->
            val delay = plan.retryDelayMs()
            val diagnostic = "Candidate ${index + 1}/${plan.candidateCount} (${run.config.encoder.mime} " +
                "${run.config.mode.width}x${run.config.mode.height}@${run.config.mode.fps}): $message"
            if (index + 1 < plan.candidateCount && delay != null) {
                onState(run.sessionId, candidateState(run, "retrying").put("reason", message).put("retryInMs", delay).apply {
                    // Encoder creation may fail before configured established
                    // this generation on the independent socket owner.
                    remove("generation")
                    put("failedGeneration", run.generation)
                })
                handler.postDelayed({ begin(candidates, index + 1, settings, token, sessionId, plan) }, delay)
            } else if (!closed && token == startToken.get()) onError(run.sessionId, problem, diagnostic)
        }
        active = run
        // Schedule before synchronous encoder calls so they do not buy extra time.
        handler.postDelayed({
            if (current(run) && !run.delivered) fail(run, "No encoded video within ${attempt.timeoutMs} ms candidate budget")
        }, (attempt.deadlineMs - SystemClock.elapsedRealtime()).coerceAtLeast(0))
        try {
            createEncoder(run)
            if (!startupCurrent(run)) return
            onState(run.sessionId, candidateState(run, "configured"))
            openCamera(run)
        } catch (exception: Exception) { fail(run, exception.message ?: "Camera startup failed", problemFor(exception)) }
    }

    private fun candidateState(run: Run, event: String) = JSONObject().put("event", event).put("generation", run.generation)
        .put("camera", run.config.camera.id).put("width", run.config.mode.width).put("height", run.config.mode.height)
        .put("fps", run.config.mode.fps).put("bitrate", run.config.mode.bitrate).put("mime", run.config.encoder.mime)
        .put("orientation", run.config.camera.orientation).put("facing", run.config.camera.facing)
        .put("attempt", run.index + 1).put("candidates", run.plan.candidateCount).put("startupBudgetMs", run.plan.budgetMs)
        .put("remainingMs", run.plan.remainingMs()).put("attemptTimeoutMs", run.attempt.timeoutMs)

    private fun startupCurrent(run: Run): Boolean {
        if (!current(run)) return false
        if (!run.delivered && SystemClock.elapsedRealtime() >= run.attempt.deadlineMs) {
            fail(run, "No encoded video within ${run.attempt.timeoutMs} ms candidate budget")
            return false
        }
        return true
    }

    fun setZoom(sessionId: String, ratio: Float) {
        require(ratio.isFinite() && ratio > 0) { "Invalid zoom ratio" }
        val token = startToken.get()
        handler.post {
            if (closed || token != startToken.get()) return@post
            val run = active
            val ready = run != null && current(run) && run.sessionId == sessionId &&
                run.camera != null && run.session != null
            runCatching {
                zoom.set(sessionId, token, ratio, ready) { repeatRequest(checkNotNull(run)) }
            }.onFailure { if (run != null) fail(run, it.message ?: "Zoom failed") }
        }
    }

    fun requestKeyFrame() = handler.post {
        active?.codec?.let { codec -> runCatching {
            codec.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } }
    }

    fun stop() {
        startToken.incrementAndGet()
        handler.post { stopInternal() }
    }
    private fun current(run: Run) = active === run && run.token == startToken.get()

    private fun createEncoder(run: Run) {
        val mode = run.config.mode
        val codec = MediaCodec.createByCodecName(run.config.encoder.name)
        run.codec = codec
        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) = Unit
            override fun onError(codec: MediaCodec, exception: MediaCodec.CodecException) {
                if (current(run)) fail(run, exception.diagnosticInfo)
            }
            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                if (!startupCurrent(run)) return
                try {
                    for (index in 0..2) {
                        val buffer = format.getByteBuffer("csd-$index") ?: continue
                        val bytes = ByteArray(buffer.remaining()).also { buffer.duplicate().get(it) }
                        onFrame(run.sessionId, 0, run.sequence++, run.generation, WireProtocol.CONFIG, annexB(bytes))
                    }
                } catch (exception: Exception) { fail(run, exception.message ?: "Invalid codec configuration") }
            }
            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                if (!startupCurrent(run)) return
                try {
                    if (info.size > 0) {
                        require(info.size <= WireProtocol.MAX_VIDEO - 20) { "Encoder frame exceeds protocol limit" }
                        val buffer = codec.getOutputBuffer(index) ?: error("Missing encoder output")
                        buffer.position(info.offset).limit(info.offset + info.size)
                        val bytes = ByteArray(info.size).also { buffer.get(it) }
                        val flags = (if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) WireProtocol.KEY_FRAME else 0) or
                            (if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) WireProtocol.CONFIG else 0)
                        onFrame(run.sessionId, info.presentationTimeUs, run.sequence++, run.generation, flags, annexB(bytes))
                        if (flags and WireProtocol.CONFIG == 0 && !run.delivered) {
                            run.delivered = true
                            onState(run.sessionId, JSONObject().put("event", "streaming").put("generation", run.generation)
                                .put("camera", run.config.camera.id).put("width", mode.width).put("height", mode.height)
                                .put("fps", mode.fps).put("bitrate", mode.bitrate).put("mime", run.config.encoder.mime)
                                .put("orientation", run.config.camera.orientation).put("facing", run.config.camera.facing)
                                .put("timestampBase", if (Build.VERSION.SDK_INT >= 33) "sensor" else "device-default"))
                        }
                    }
                } catch (exception: Exception) { fail(run, exception.message ?: "Encoder output failed") }
                finally { runCatching { codec.releaseOutputBuffer(index, false) } }
            }
        }, handler)
        val format = MediaFormat.createVideoFormat(run.config.encoder.mime, mode.width, mode.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, android.media.MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, mode.bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, mode.fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        }
        codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        run.surface = codec.createInputSurface()
        codec.start()
    }

    @SuppressLint("MissingPermission") // Service checks permission before constructing or starting the engine.
    private fun openCamera(run: Run) {
        catalog.manager.openCamera(run.config.camera.deviceId, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                if (!startupCurrent(run)) { camera.close(); return }
                run.camera = camera
                try {
                    val output = OutputConfiguration(run.surface!!)
                    run.config.camera.physicalId?.let(output::setPhysicalCameraId)
                    if (Build.VERSION.SDK_INT >= 33) output.timestampBase = OutputConfiguration.TIMESTAMP_BASE_SENSOR
                    camera.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                        listOf(output), executor, object : CameraCaptureSession.StateCallback() {
                            override fun onConfigured(session: CameraCaptureSession) {
                                if (!startupCurrent(run)) { session.close(); return }
                                run.session = session
                                runCatching { repeatRequest(run) }.onFailure { fail(run, it.message ?: "Capture request failed") }
                            }
                            override fun onConfigureFailed(session: CameraCaptureSession) {
                                session.close()
                                if (current(run)) fail(run, "Camera rejected the recording session")
                            }
                        }))
                } catch (exception: Exception) { fail(run, exception.message ?: "Camera session failed", problemFor(exception)) }
            }
            // Disconnection usually means a higher-priority camera client took the device.
            override fun onDisconnected(camera: CameraDevice) {
                camera.close()
                if (current(run)) fail(run, "Camera disconnected", HostProblem.CameraBusy)
            }
            override fun onError(camera: CameraDevice, error: Int) {
                camera.close()
                if (current(run)) fail(run, "Camera error $error", problemForDeviceError(error))
            }
        }, handler)
    }

    private fun repeatRequest(run: Run) {
        if (!startupCurrent(run)) return
        val chars = run.config.characteristics
        val settings = run.settings
        val request = run.camera!!.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
            addTarget(run.surface!!)
            set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, run.config.fpsRange)
            val zoomRange = if (run.config.camera.physicalId == null) chars[CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE] else null
            set(CaptureRequest.CONTROL_ZOOM_RATIO,
                zoom.value(run.sessionId, run.token).coerceIn(zoomRange?.lower ?: 1f, zoomRange?.upper ?: 1f))
            val afModes = chars[CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES]?.toList().orEmpty()
            val af = if (settings.autofocus && CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes)
                CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO else CaptureRequest.CONTROL_AF_MODE_OFF
            if (af in afModes) set(CaptureRequest.CONTROL_AF_MODE, af)
            if (!settings.autofocus) set(CaptureRequest.LENS_FOCUS_DISTANCE, 0f)
            val exposure = chars[CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE]
            set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION,
                settings.exposure.coerceIn(exposure?.lower ?: 0, exposure?.upper ?: 0))
            if (chars[CameraCharacteristics.FLASH_INFO_AVAILABLE] == true)
                set(CaptureRequest.FLASH_MODE, if (settings.torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF)
            val ois = chars[CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION]?.toList().orEmpty()
            val eis = chars[CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES]?.toList().orEmpty()
            val optical = if (settings.stabilization == "optical" && CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON in ois)
                CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON else CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_OFF
            val electronic = if (settings.stabilization == "electronic" && CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON in eis)
                CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON else CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF
            if (optical in ois) set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, optical)
            if (electronic in eis) set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, electronic)
        }.build()
        run.session!!.setRepeatingRequest(request, object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(session: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
                if (!current(run)) return
                val crop = result[CaptureResult.SCALER_CROP_REGION]
                onMetadata(run.sessionId, JSONObject().put("generation", run.generation).put("frameNumber", result.frameNumber)
                    .put("sensorTimestampNs", result[CaptureResult.SENSOR_TIMESTAMP] ?: 0L)
                    .put("zoom", result[CaptureResult.CONTROL_ZOOM_RATIO] ?: 1f)
                    .put("physicalCamera", run.config.camera.physicalId ?: result[CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID] ?: run.config.camera.deviceId)
                    .put("exposureNs", result[CaptureResult.SENSOR_EXPOSURE_TIME] ?: 0L)
                    .put("exposureCompensationSteps", result[CaptureResult.CONTROL_AE_EXPOSURE_COMPENSATION] ?: 0)
                    .put("iso", result[CaptureResult.SENSOR_SENSITIVITY] ?: 0)
                    .put("afState", result[CaptureResult.CONTROL_AF_STATE] ?: 0)
                    .put("aeState", result[CaptureResult.CONTROL_AE_STATE] ?: 0)
                    .put("torchAvailable", run.config.characteristics[CameraCharacteristics.FLASH_INFO_AVAILABLE] == true)
                    .put("torchEnabled", result[CaptureResult.FLASH_MODE] == CaptureRequest.FLASH_MODE_TORCH)
                    .put("focusDistance", result[CaptureResult.LENS_FOCUS_DISTANCE] ?: 0f)
                    .put("ois", result[CaptureResult.LENS_OPTICAL_STABILIZATION_MODE] ?: 0)
                    .put("eis", result[CaptureResult.CONTROL_VIDEO_STABILIZATION_MODE] ?: 0)
                    .put("crop", JSONArray(if (crop == null) emptyList<Int>() else listOf(crop.left, crop.top, crop.right, crop.bottom))))
            }
        }, handler)
    }

    private fun fail(run: Run, message: String, problem: HostProblem = HostProblem.CameraFailed) {
        if (!current(run)) return
        stopInternal()
        if (!run.delivered) run.startupFailure?.invoke(problem, message) else onError(run.sessionId, problem, message)
    }

    private fun stopInternal() {
        val run = active ?: return
        active = null
        runCatching { run.session?.stopRepeating() }
        runCatching { run.session?.close() }
        runCatching { run.camera?.close() }
        runCatching { run.codec?.stop() }
        runCatching { run.codec?.release() }
        runCatching { run.surface?.release() }
    }

    override fun close() {
        startToken.incrementAndGet()
        handler.post { closed = true; stopInternal(); thread.quitSafely() }
    }
}
