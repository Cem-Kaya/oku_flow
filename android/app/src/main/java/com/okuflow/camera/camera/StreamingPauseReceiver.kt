package com.okuflow.camera.camera

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.okuflow.camera.settings.StreamingIntentStore

/** Non-exported notification action writes Pause before closing the listener/camera service. */
class StreamingPauseReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_PAUSE) return
        pauseStreaming(context)
    }
    companion object {
        const val ACTION_PAUSE = "com.okuflow.camera.PAUSE_STREAMING"
        fun pauseStreaming(context: Context) {
            StreamingIntentStore.get(context).pause()
            context.stopService(Intent(context, CameraStreamService::class.java))
        }
    }
}
