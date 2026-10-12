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
import com.okuflow.camera.settings.StreamingIntentStore
import com.okuflow.camera.setup.DeviceSetupMonitor
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import com.okuflow.camera.setup.listenerStopDelayMs
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean

/** Starts only from the visible app after prerequisites; camera runs only for an active USB host. */
class CameraStreamService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private lateinit var engine: CameraEngine
    private lateinit var server: UsbSocketServer
    private lateinit var monitor: DeviceSetupMonitor
    private lateinit var store: CameraSettingsStore
    private lateinit var power: PowerManager
    private lateinit var streamingIntent: StreamingIntentStore
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
            server.error("Phone is too hot; streaming stopped. Let it cool, then start again.", HostProblem.TooHot.wireCode)
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
        streamingIntent = StreamingIntentStore.get(this)
        if (!streamingIntent.canStart(true)) { stopSelf(); return }
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
        store = CameraSettingsStore.get(this)
        store.update(store.state.value.forNewHostSession())
        monitor = DeviceSetupMonitor(this).also { it.start() }
        power = getSystemService(PowerManager::class.java)
        thermalFps = if (power.currentThermalStatus >= PowerManager.THERMAL_STATUS_SEVERE) 30 else 60
        val catalog = CameraCatalog(this)
        engine = CameraEngine(catalog,
            onState = { state ->
                if (!streamingIntent.canStart(true)) return@CameraEngine
                if (!::server.isInitialized || !server.connected()) return@CameraEngine
                if (!server.state(state)) return@CameraEngine
                if (state.optString("event") == "streaming") {
                    HostLink.report(null)
                    HostLink.publish(HostLinkState.Streaming)
                }
                val codec = if (state.optString("mime") == "video/hevc") "HEVC" else "H.264"
                HostLink.details.value = "${state.optInt("width")} × ${state.optInt("height")} · ${state.optInt("fps")} FPS · $codec · ${state.optInt("bitrate") / 1_000_000} Mbps"
                if (state.optString("event") == "streaming")
                    updateNotification(getString(R.string.service_streaming_notification, HostLink.details.value))
            }, onMetadata = { server.metadata(it) }, onFrame = { pts, seq, generation, flags, bytes ->
                server.frame(pts, seq, generation, flags, bytes)
            }, onError = { problem, message ->
                lastSettings = null
                publishIdle()
                HostLink.report(problem)
                server.error(message, problem.wireCode)
            })
        try {
            server = UsbSocketServer(catalog::capabilities, ::command, onConnect = {
                store.update(store.state.value.forNewHostSession())
                // A new host session is a fresh attempt; its own failure reports again.
                if (HostLink.problem.value != HostProblem.TooHot) HostLink.report(null)
                HostLink.publish(HostLinkState.Connected)
                updateNotification(getString(R.string.service_connected_notification))
            }, problem = { HostLink.problem.value?.wireCode }, onListenerFailed = {
                // The resumed activity retries; a dead listener must not leave a silent running service.
                mainExecutor.execute { HostLink.report(HostProblem.ServiceFailed); stopSelf() }
            }) {
                requested = false
                lastSettings = null
                store.update(store.state.value.forNewHostSession())
                engine.stop()
                HostLink.publish(HostLinkState.Disconnected)
                HostLink.details.value = ""
                updateNotification(getString(R.string.service_notification))
            }
        } catch (exception: Exception) {
            HostLink.report(HostProblem.ServiceFailed)
            stopSelf()
            return
        }
        // A fresh listener starts without the previous attempt's problem; heat persists until cooling.
        // Cleared only once listening, so periodic retries of a failing listener do not flicker.
        if (HostLink.problem.value != HostProblem.TooHot) HostLink.report(null)
        power.addThermalStatusListener(mainExecutor, thermalListener)
        scope.launch {
            monitor.snapshot.collectLatest { snapshot ->
                val stopAfter = listenerStopDelayMs(snapshot) ?: return@collectLatest
                // collectLatest cancels this wait if the cable reports connected again.
                kotlinx.coroutines.delay(stopAfter)
                // Unplugging ends this attempt; the next cable connection starts clean.
                if (!snapshot.usbConnectedToComputer) HostLink.report(null)
                stopSelf()
            }
        }
        scope.launch { store.state.collectLatest { kotlinx.coroutines.delay(350); restart() } }
        scope.launch { streamingIntent.paused.collectLatest { paused -> if (paused) stopSelf() } }
        running.set(true)
    }

    private fun command(command: JSONObject) {
        if (!streamingIntent.canStart(true)) return
        try {
            when (command.getString("op")) {
                "start" -> {
                    if (power.currentThermalStatus >= PowerManager.THERMAL_STATUS_CRITICAL) {
                        HostLink.report(HostProblem.TooHot)
                        server.error("Phone is too hot", HostProblem.TooHot.wireCode)
                        return
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

    // Settings, thermal and command threads all restart; serialize the compare-and-start.
    @Synchronized private fun restart() {
        if (!streamingIntent.canStart(true) || !requested || !::server.isInitialized || server.hostCodecs().isEmpty()) return
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
        updateNotification(getString(if (::server.isInitialized && server.connected())
            R.string.service_connected_notification else R.string.service_notification))
    }

    @android.annotation.SuppressLint("MissingPermission") // FGS is permitted without the optional drawer permission.
    private fun updateNotification(text: String) {
        runCatching { getSystemService(NotificationManager::class.java).notify(1, notification(text)) }
    }

    private fun notification(text: String) = NotificationCompat.Builder(this, "camera")
        .setSmallIcon(R.drawable.ic_camera_notification).setContentTitle(getString(R.string.app_name))
        .setContentText(text).setOngoing(true)
        .setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE))
        .addAction(R.drawable.ic_camera_notification, getString(R.string.action_pause_streaming),
            PendingIntent.getBroadcast(this, 1, Intent(this, StreamingPauseReceiver::class.java)
                .setAction(StreamingPauseReceiver.ACTION_PAUSE), PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        .build()

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int) = START_NOT_STICKY
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onDestroy() {
        running.set(false)
        scope.cancel()
        if (::power.isInitialized) power.removeThermalStatusListener(thermalListener)
        if (::monitor.isInitialized) monitor.stop()
        if (::server.isInitialized) server.close()
        if (::engine.isInitialized) engine.close()
        stopForeground(STOP_FOREGROUND_REMOVE)
        HostLink.details.value = ""
        HostLink.publish(HostLinkState.Disconnected)
        super.onDestroy()
    }
    companion object {
        private val running = AtomicBoolean(false)

        /** True from a successful listener start until onDestroy; the activity retries while false. */
        val isRunning: Boolean get() = running.get()

        fun startVisible(context: Context) {
            if (!StreamingIntentStore.get(context).canStart(true)) return
            runCatching { androidx.core.content.ContextCompat.startForegroundService(context,
                Intent(context, CameraStreamService::class.java)) }.onFailure { HostLink.report(HostProblem.ServiceFailed) }
        }
    }
}
