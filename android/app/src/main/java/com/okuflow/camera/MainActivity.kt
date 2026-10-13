package com.okuflow.camera

import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.okuflow.camera.settings.AppLanguageStore
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
        setContent {
            OkuFlowCameraTheme {
                OkuFlowCameraApp()
            }
        }
    }
}
