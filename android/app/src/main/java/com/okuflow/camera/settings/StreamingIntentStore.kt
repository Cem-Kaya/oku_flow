package com.okuflow.camera.settings

import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow

/** Small synchronous writes occur on user actions only, never on the camera/frame path. */
class StreamingIntentStore private constructor(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences("streaming_intent", Context.MODE_PRIVATE)
    private val intent = StreamingIntent(object : StreamingPausePersistence {
        override fun readPaused() = preferences.getBoolean("paused", false)
        @android.annotation.SuppressLint("UseKtx") // The Boolean commit result is required to fail closed on storage errors.
        override fun writePaused(paused: Boolean) = preferences.edit().putBoolean("paused", paused).commit()
    })
    private val mutablePaused = MutableStateFlow(intent.paused)
    val paused = mutablePaused.asStateFlow()
    private val mutableSaveFailed = MutableStateFlow(false)
    val saveFailed = mutableSaveFailed.asStateFlow()

    fun canStart(prerequisitesReady: Boolean) = intent.canStart(prerequisitesReady)

    @Synchronized fun pause() {
        mutableSaveFailed.value = !intent.pause()
        mutablePaused.value = intent.paused
    }

    @Synchronized fun resume(): Boolean {
        val saved = intent.resume()
        mutableSaveFailed.value = !saved
        mutablePaused.value = intent.paused
        return saved
    }

    companion object {
        @Volatile private var instance: StreamingIntentStore? = null
        fun get(context: Context): StreamingIntentStore = instance ?: synchronized(this) {
            instance ?: StreamingIntentStore(context).also { instance = it }
        }
    }
}
