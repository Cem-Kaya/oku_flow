package com.okuflow.camera.camera

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import com.okuflow.camera.MainActivity
import com.okuflow.camera.R
import com.okuflow.camera.link.HostLink
import com.okuflow.camera.link.HostLinkState
import com.okuflow.camera.link.HostProblem
import com.okuflow.camera.link.UsbSocketServer
import com.okuflow.camera.settings.CameraSettingsStore
import com.okuflow.camera.settings.CameraSettings
import com.okuflow.camera.setup.DeviceSetupMonitor
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import org.json.JSONObject

/** Starts only from the visible app after prerequisites; camera runs only for an active USB host. */
class CameraStreamService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private lateinit var engine: CameraEngine
    private lateinit var server: UsbSocketServer
    private lateinit var monitor: DeviceSetupMonitor
    private lateinit var store: CameraSettingsStore
    private lateinit var power: PowerManager
    @Volatile private var requested = false
    @Volatile private var thermalFps = 60
    @Volatile private var lastSettings: CameraSettings? = null
    @Volatile private var lastThermalFps = 0
    private val thermalListener = PowerManager.OnThermalStatusChangedListener { status ->
        val next = if (status >= PowerManager.THERMAL_STATUS_SEVERE) 30 else 60
        if (status >= PowerManager.THERMAL_STATUS_CRITICAL) {
            requested = false
            lastSettings = null
            engine.stop()
            publishIdle()
            HostLink.report(HostProblem.TooHot)
            server.error("Phone is too hot; streaming stopped. Let it cool, then start again.")
        } else {
            // Cooling below CRITICAL makes a new start possible; drop the stale warning.
            if (HostLink.problem.value == HostProblem.TooHot) HostLink.report(null)
            if (next != thermalFps) {
                thermalFps = next
                restart()
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        val notifications = getSystemService(NotificationManager::class.java)
        notifications.createNotificationChannel(NotificationChannel("camera", getString(R.string.app_name), NotificationManager.IMPORTANCE_LOW))
        try {
            ServiceCompat.startForeground(this, 1, notification(getString(R.string.service_notification)),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        } catch (exception: Exception) {
            HostLink.report(HostProblem.ServiceFailed)
            stopSelf()
            return
        }
        // A fresh listener starts without the previous attempt's problem; heat persists until cooling.
        if (HostLink.problem.value != HostProblem.TooHot) HostLink.report(null)
        store = CameraSettingsStore.get(this)
        store.update(store.state.value.forNewHostSession())
        monitor = DeviceSetupMonitor(this).also { it.start() }
        power = getSystemService(PowerManager::class.java)
        thermalFps = if (power.currentThermalStatus >= PowerManager.THERMAL_STATUS_SEVERE) 30 else 60
        val catalog = CameraCatalog(this)
        engine = CameraEngine(catalog,
            onState = { state ->
                if (!::server.isInitialized || !server.connected()) return@CameraEngine
                if (!server.state(state)) return@CameraEngine
                if (state.optString("event") == "streaming") {
                    HostLink.report(null)
                    HostLink.publish(HostLinkState.Streaming)
                }
                val codec = if (state.optString("mime") == "video/hevc") "HEVC" else "H.264"
                HostLink.details.value = "${state.optInt("width")} × ${state.optInt("height")} · ${state.optInt("fps")} FPS · $codec · ${state.optInt("bitrate") / 1_000_000} Mbps"
            }, onMetadata = { server.metadata(it) }, onFrame = { pts, seq, generation, flags, bytes ->
                server.frame(pts, seq, generation, flags, bytes)
            }, onError = { problem, message ->
                lastSettings = null
                publishIdle()
                HostLink.report(problem)
                server.error(message)
            })
        try {
            server = UsbSocketServer(catalog::capabilities, ::command, onConnect = {
                store.update(store.state.value.forNewHostSession())
                // A new host session is a fresh attempt; its own failure reports again.
                if (HostLink.problem.value != HostProblem.TooHot) HostLink.report(null)
                HostLink.publish(HostLinkState.Connected)
            }) {
                requested = false
                lastSettings = null
                store.update(store.state.value.forNewHostSession())
                engine.stop()
                HostLink.publish(HostLinkState.Disconnected)
                HostLink.details.value = ""
            }
        } catch (exception: Exception) {
            HostLink.report(HostProblem.ServiceFailed)
            stopSelf()
            return
        }
        power.addThermalStatusListener(mainExecutor, thermalListener)
        scope.launch {
            monitor.snapshot.collectLatest { snapshot ->
                if (!snapshot.canListenForHost) {
                    // Unplugging ends this attempt; the next cable connection starts clean.
                    if (!snapshot.usbConnectedToComputer) HostLink.report(null)
                    stopSelf()
                }
            }
        }
        scope.launch { store.state.collectLatest { kotlinx.coroutines.delay(350); restart() } }
    }

    private fun command(command: JSONObject) {
        try {
            when (command.getString("op")) {
                "start" -> {
                    if (power.currentThermalStatus >= PowerManager.THERMAL_STATUS_CRITICAL) {
                        HostLink.report(HostProblem.TooHot)
                        error("Phone is too hot")
                    }
                    requested = true
                    restart()
                }
                "stop" -> {
                    requested = false
                    lastSettings = null
                    store.update(store.state.value.forNewHostSession())
                    engine.stop()
                    publishIdle()
                }
                "zoom" -> engine.setZoom(command.getDouble("ratio").toFloat())
                "camera" -> {
                    val id = if (command.has("cameraId")) command.getString("cameraId") else command.getString("id")
                    require(CameraCatalog(this).cameras().any { it.id == id }) { "Unknown camera" }
                    store.update(store.state.value.copy(cameraId = id))
                }
                "torch" -> store.update(store.state.value.copy(torch = command.getBoolean("enabled")))
                "exposure" -> store.update(store.state.value.copy(exposure = command.getInt("steps")))
                "autofocus" -> store.update(store.state.value.copy(autofocus = command.getBoolean("enabled")))
                "keyframe" -> engine.requestKeyFrame()
                else -> error("Unsupported command")
            }
            server.state(JSONObject().put("event", "commandAccepted").put("id", command.optLong("id")))
        } catch (exception: Exception) { server.error(exception.message ?: "Command failed") }
    }

    private fun restart() {
        if (!requested || !::server.isInitialized || server.hostCodecs().isEmpty()) return
        val settings = store.state.value
        if (settings == lastSettings && thermalFps == lastThermalFps) return
        lastSettings = settings
        lastThermalFps = thermalFps
        publishIdle()
        engine.start(settings, server.hostCodecs(), thermalFps, server.hostLimits())
    }

    /** Not streaming: Connected while OkuFlow still holds the session, otherwise Disconnected. */
    private fun publishIdle() {
        HostLink.details.value = ""
        HostLink.publish(if (::server.isInitialized && server.connected()) HostLinkState.Connected else HostLinkState.Disconnected)
    }

    private fun notification(text: String) = NotificationCompat.Builder(this, "camera")
        .setSmallIcon(R.mipmap.ic_launcher).setContentTitle(getString(R.string.app_name))
        .setContentText(text).setOngoing(true)
        .setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE))
        .build()

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int) = START_NOT_STICKY
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onDestroy() {
        scope.cancel()
        if (::power.isInitialized) power.removeThermalStatusListener(thermalListener)
        if (::monitor.isInitialized) monitor.stop()
        if (::server.isInitialized) server.close()
        if (::engine.isInitialized) engine.close()
        HostLink.details.value = ""
        HostLink.publish(HostLinkState.Disconnected)
        super.onDestroy()
    }
    companion object {
        fun startVisible(context: Context) {
            runCatching { androidx.core.content.ContextCompat.startForegroundService(context,
                Intent(context, CameraStreamService::class.java)) }.onFailure { HostLink.report(HostProblem.ServiceFailed) }
        }
    }
}
