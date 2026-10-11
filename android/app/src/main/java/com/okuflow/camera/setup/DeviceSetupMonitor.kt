package com.okuflow.camera.setup

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.database.ContentObserver
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Publishes a live [DeviceSetupSnapshot].
 *
 * Developer options and USB debugging are observed through their
 * `Settings.Global` URIs, so the screen advances the moment the user flips
 * the switch and returns. Runtime permission changes have no observer;
 * callers invoke [refresh] when the activity resumes.
 */
class DeviceSetupMonitor(context: Context) {
    private val appContext = context.applicationContext
    private val mainHandler = Handler(Looper.getMainLooper())

    // Latest value of the USB_STATE "connected" extra; null until the first
    // broadcast arrives, in which case the battery plug type is used instead.
    private var usbStateConnected: Boolean? = null
    private var usbAdbActive = false
    private var batteryPluggedUsb = false
    private var started = false

    private val mutableSnapshot = MutableStateFlow(read())
    val snapshot: StateFlow<DeviceSetupSnapshot> = mutableSnapshot.asStateFlow()

    private val settingsObserver = object : ContentObserver(mainHandler) {
        override fun onChange(selfChange: Boolean) = refresh()
    }

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            applyBroadcast(intent)
            refresh()
        }
    }

    fun start() {
        if (started) return
        started = true
        val resolver = appContext.contentResolver
        resolver.registerContentObserver(
            Settings.Global.getUriFor(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED),
            false,
            settingsObserver,
        )
        resolver.registerContentObserver(
            Settings.Global.getUriFor(Settings.Global.ADB_ENABLED),
            false,
            settingsObserver,
        )
        val filter = IntentFilter().apply {
            addAction(ACTION_USB_STATE)
            addAction(Intent.ACTION_BATTERY_CHANGED)
        }
        ContextCompat.registerReceiver(
            appContext,
            usbReceiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        // Receiver callbacks can wait for the main looper. Seed both sticky
        // values synchronously so service startup sees the connected cable.
        for (action in listOf(ACTION_USB_STATE, Intent.ACTION_BATTERY_CHANGED)) {
            ContextCompat.registerReceiver(
                appContext, null, IntentFilter(action), ContextCompat.RECEIVER_NOT_EXPORTED,
            )?.let(::applyBroadcast)
        }
        refresh()
    }

    fun stop() {
        if (!started) return
        started = false
        appContext.contentResolver.unregisterContentObserver(settingsObserver)
        appContext.unregisterReceiver(usbReceiver)
    }

    fun refresh() {
        mutableSnapshot.value = read()
    }

    private fun applyBroadcast(intent: Intent) {
        when (intent.action) {
            ACTION_USB_STATE -> {
                if (intent.hasExtra(EXTRA_USB_CONNECTED)) {
                    usbStateConnected = intent.getBooleanExtra(EXTRA_USB_CONNECTED, false)
                }
                usbAdbActive = usbStateConnected == true && intent.getBooleanExtra("adb", false)
            }
            Intent.ACTION_BATTERY_CHANGED ->
                batteryPluggedUsb =
                    intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ==
                        BatteryManager.BATTERY_PLUGGED_USB
        }
    }

    private fun read(): DeviceSetupSnapshot {
        return DeviceSetupSnapshot(
            developerOptionsEnabled =
                readDebuggingSetting(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED),
            usbDebuggingEnabled =
                readDebuggingSetting(Settings.Global.ADB_ENABLED),
            cameraPermissionGranted =
                ContextCompat.checkSelfPermission(appContext, Manifest.permission.CAMERA) ==
                    PackageManager.PERMISSION_GRANTED,
            // USB_STATE distinguishes a computer (USB host) from a wall
            // charger; the battery plug type is only a coarse fallback.
            usbConnectedToComputer = usbStateConnected ?: batteryPluggedUsb,
            usbAdbActive = usbAdbActive,
        )
    }

    private fun readDebuggingSetting(key: String): Boolean? = debuggingSettingEnabled(
        runCatching { Settings.Global.getInt(appContext.contentResolver, key, -1) }.getOrDefault(-1),
        settingsRedacted = Build.VERSION.SDK_INT >= 37,
    )

    private companion object {
        // UsbManager.ACTION_USB_STATE / USB_CONNECTED are not in the public
        // SDK, but the sticky broadcast itself has been stable since API 12.
        const val ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE"
        const val EXTRA_USB_CONNECTED = "connected"
    }
}
