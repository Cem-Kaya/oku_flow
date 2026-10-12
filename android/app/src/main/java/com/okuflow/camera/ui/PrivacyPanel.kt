package com.okuflow.camera.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import com.okuflow.camera.R
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.edit
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

const val PRIVACY_POLICY_URL = "https://github.com/Cem-Kaya/oku_flow/blob/main/android/PRIVACY.md"
const val PRIVACY_CONTACT_URL = "https://github.com/Cem-Kaya/oku_flow/issues"

/** Offline policy remains readable without a browser or network connection. */
@Composable
fun PrivacyPanel(onBack: () -> Unit) {
    val uriHandler = LocalUriHandler.current
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background,
        contentColor = MaterialTheme.colorScheme.onBackground) {
        Column(Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(20.dp),
            horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(20.dp)) {
            OutlinedButton(onClick = onBack, shape = RoundedCornerShape(12.dp), border = BorderStroke(2.dp, Color.White),
                modifier = Modifier.widthIn(max = 680.dp).fillMaxWidth().heightIn(min = 56.dp)) {
                Text(stringResource(R.string.action_back_settings))
            }
            Text(stringResource(R.string.privacy_title), fontSize = 28.sp, lineHeight = 36.sp,
                fontWeight = FontWeight.Bold, modifier = Modifier.semantics { heading() })
            for (resource in listOf(R.string.privacy_identity,
                R.string.privacy_access, R.string.privacy_retention,
                R.string.privacy_windows, R.string.privacy_usb,
                R.string.privacy_contact)) {
                Text(stringResource(resource), fontSize = 20.sp, lineHeight = 28.sp, textAlign = TextAlign.Start,
                    modifier = Modifier.widthIn(max = 680.dp).fillMaxWidth())
            }
            OutlinedButton(onClick = { runCatching { uriHandler.openUri(PRIVACY_POLICY_URL) } },
                modifier = Modifier.widthIn(max = 680.dp).fillMaxWidth().heightIn(min = 56.dp)) {
                Text(stringResource(R.string.privacy_public))
            }
            OutlinedButton(onClick = { runCatching { uriHandler.openUri(PRIVACY_CONTACT_URL) } },
                modifier = Modifier.widthIn(max = 680.dp).fillMaxWidth().heightIn(min = 56.dp)) {
                Text(stringResource(R.string.privacy_contact_action))
            }
        }
    }
}

/** Explicit, once per installation; denial never affects USB discovery or camera service starts. */
@Composable
fun OptionalNotificationControl() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
    val context = LocalContext.current
    val preferences = remember(context) { context.getSharedPreferences("setup", Context.MODE_PRIVATE) }
    var prompted by remember { mutableStateOf(preferences.getBoolean("notification_prompted", false)) }
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    if (prompted || context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(stringResource(R.string.notification_optional_hint), fontSize = 18.sp, lineHeight = 26.sp)
        OutlinedButton(onClick = {
            preferences.edit { putBoolean("notification_prompted", true) }
            prompted = true
            permissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }, modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp)) {
            Text(stringResource(R.string.notification_optional_action))
        }
    }
}
