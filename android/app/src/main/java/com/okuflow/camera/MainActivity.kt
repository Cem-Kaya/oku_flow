package com.okuflow.camera

import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.okuflow.camera.camera.CameraStreamService
import com.okuflow.camera.link.HOST_LAUNCH_EXTRA
import com.okuflow.camera.link.PausedStatusEndpoint
import com.okuflow.camera.link.showOverKeyguardForHostLaunch
import com.okuflow.camera.settings.AppLanguageStore
import com.okuflow.camera.settings.StreamingIntentStore
import com.okuflow.camera.ui.OkuFlowCameraApp
import com.okuflow.camera.ui.theme.OkuFlowCameraTheme

class MainActivity : ComponentActivity() {
    // Android 11–12 per-app language; Android 13+ applies it in the framework.
    override fun attachBaseContext(newBase: Context) = super.attachBaseContext(AppLanguageStore.wrap(newBase))

    override fun onCreate(savedInstanceState: Bundle?) {
        // Always-dark UI: light system bar icons on the black background.
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        super.onCreate(savedInstanceState)
        // Covers setup, permission-return, settings and streaming alike.
        // Android stops honoring this window flag when the app is backgrounded.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        PausedStatusEndpoint.start(this)
        // A recreated activity (configuration change, or restore after the
        // process was killed) is not a new computer launch: the system hands
        // back the original intent, extra included.
        if (savedInstanceState == null) applyHostLaunch(intent) else intent?.removeExtra(HOST_LAUNCH_EXTRA)
        setContent {
            OkuFlowCameraTheme {
                OkuFlowCameraApp()
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        applyHostLaunch(intent)
    }

    // Leaving the screen ends the computer launch: the next lock screen is
    // ordinary again, and a later recreation is not a host launch.
    override fun onStop() {
        super.onStop()
        if (!isChangingConfigurations) {
            setShowWhenLocked(false)
            setTurnScreenOn(false)
        }
    }

    private fun applyHostLaunch(intent: Intent?) {
        val hostLaunch = intent?.getBooleanExtra(HOST_LAUNCH_EXTRA, false) == true
        intent?.removeExtra(HOST_LAUNCH_EXTRA)
        if (!hostLaunch) return
        CameraStreamService.onHostLaunch()
        val show = showOverKeyguardForHostLaunch(true, StreamingIntentStore.get(this).paused.value)
        setShowWhenLocked(show)
        setTurnScreenOn(show)
    }
}
