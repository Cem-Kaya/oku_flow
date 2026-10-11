package com.okuflow.camera.setup

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings

/**
 * One-tap jumps into the system screens the user needs. Each target falls
 * back to a broader settings screen when an OEM build does not implement
 * the specific action.
 */
object SystemSettingsLauncher {

    /** "About phone" — where Build number is tapped seven times. */
    fun openAboutPhone(context: Context) {
        launchFirst(
            context,
            Intent(Settings.ACTION_DEVICE_INFO_SETTINGS),
            Intent(Settings.ACTION_SETTINGS),
        )
    }

    /** Developer options — where USB debugging is switched on. */
    fun openDeveloperOptions(context: Context) {
        launchFirst(
            context,
            Intent(Settings.ACTION_APPLICATION_DEVELOPMENT_SETTINGS),
            Intent(Settings.ACTION_DEVICE_INFO_SETTINGS),
            Intent(Settings.ACTION_SETTINGS),
        )
    }

    /** Privacy settings, home of the system-wide camera access switch on Android 12+. */
    fun openPrivacySettings(context: Context) {
        launchFirst(
            context,
            Intent(Settings.ACTION_PRIVACY_SETTINGS),
            Intent(Settings.ACTION_SETTINGS),
        )
    }

    /** This app's details page, used once camera permission was permanently denied. */
    fun openAppDetails(context: Context) {
        launchFirst(
            context,
            Intent(
                Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", context.packageName, null),
            ),
            Intent(Settings.ACTION_SETTINGS),
        )
    }

    private fun launchFirst(context: Context, vararg candidates: Intent) {
        for (intent in candidates) {
            try {
                context.startActivity(intent)
                return
            } catch (notFound: ActivityNotFoundException) {
                // Try the next, broader screen.
            } catch (denied: SecurityException) {
                // Some OEM builds guard specific settings activities.
            }
        }
    }
}
