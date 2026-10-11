package com.okuflow.camera.ui

import android.Manifest
import android.os.Build
import android.view.HapticFeedbackConstants
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.StringRes
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.okuflow.camera.R
import com.okuflow.camera.link.HostProblem
import com.okuflow.camera.setup.OemFamily
import com.okuflow.camera.setup.SetupStep
import com.okuflow.camera.setup.SetupViewModel
import com.okuflow.camera.setup.SystemSettingsLauncher

/**
 * The main camera pane shows one step, with a separate optional Advanced menu:
 * developer options -> USB debugging -> camera permission -> plug in ->
 * open OkuFlow -> connected -> streaming, or a named problem with its remedy.
 * Returning from system settings re-reads the state, so the screen advances
 * by itself.
 */
@Composable
fun OkuFlowCameraApp(viewModel: SetupViewModel = viewModel()) {
    val step by viewModel.step.collectAsStateWithLifecycle()
    val cameraPermanentlyDenied by viewModel.cameraPermanentlyDenied.collectAsStateWithLifecycle()
    val settings by viewModel.settings.collectAsStateWithLifecycle()
    val diagnostics by viewModel.diagnostics.collectAsStateWithLifecycle()
    val problem by viewModel.problem.collectAsStateWithLifecycle()
    var showSettings by rememberSaveable { mutableStateOf(false) }
    val context = LocalContext.current
    val activity = LocalActivity.current
    val lifecycle = LocalLifecycleOwner.current.lifecycle

    LifecycleResumeEffect(Unit) {
        viewModel.refresh()
        onPauseOrDispose {}
    }

    val cameraPermissionLauncher =
        rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            viewModel.onCameraPermissionResult(
                granted = granted,
                shouldShowRationale =
                    activity?.shouldShowRequestPermissionRationale(Manifest.permission.CAMERA) ?: true,
            )
        }

    // Ask for the camera once, automatically, when that step first appears:
    // the user arrives here straight from enabling USB debugging and the
    // permission dialog is the obvious next thing. Later taps retry.
    var cameraAutoPrompted by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(step) {
        if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.RESUMED))
            viewModel.startServiceIfReady()
        if (step == SetupStep.AllowCamera && !cameraAutoPrompted && !cameraPermanentlyDenied) {
            cameraAutoPrompted = true
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    StepChangeHaptics(step)

    val openSettings = stringResource(R.string.action_open_settings)
    val content = when (step) {
        SetupStep.EnableDeveloperOptions -> StepContent(
            title = stringResource(R.string.developer_title),
            hint = stringResource(developerHintFor(OemFamily.fromManufacturer(Build.MANUFACTURER))),
            actionLabel = openSettings,
            onAction = { SystemSettingsLauncher.openAboutPhone(context) },
        )

        SetupStep.EnableUsbDebugging -> StepContent(
            title = stringResource(R.string.usb_debugging_title),
            hint = stringResource(R.string.usb_debugging_hint),
            actionLabel = openSettings,
            onAction = { SystemSettingsLauncher.openDeveloperOptions(context) },
        )

        SetupStep.AllowCamera -> StepContent(
            title = stringResource(R.string.camera_title),
            hint = stringResource(if (cameraPermanentlyDenied) R.string.camera_hint_settings else R.string.camera_hint),
            actionLabel = if (cameraPermanentlyDenied) openSettings else stringResource(R.string.action_allow_camera),
            onAction = {
                if (cameraPermanentlyDenied) {
                    SystemSettingsLauncher.openAppDetails(context)
                } else {
                    cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
                }
            },
        )

        SetupStep.ConnectUsb -> StepContent(
            title = stringResource(R.string.connect_usb_title),
            hint = stringResource(R.string.connect_usb_hint),
        )

        SetupStep.CheckUsbSetup -> StepContent(
            title = stringResource(R.string.check_usb_title),
            hint = stringResource(R.string.check_usb_hint),
            actionLabel = openSettings,
            onAction = { SystemSettingsLauncher.openDeveloperOptions(context) },
        )

        SetupStep.OpenOkuFlow -> StepContent(
            title = stringResource(R.string.open_okuflow_title),
            hint = stringResource(R.string.open_okuflow_hint),
        )

        SetupStep.HostConnected -> StepContent(
            title = stringResource(R.string.connected_title),
            hint = stringResource(R.string.connected_hint),
        )

        SetupStep.Problem -> {
            val shown = problem ?: HostProblem.CameraFailed
            StepContent(
                title = stringResource(problemTitle(shown)),
                hint = stringResource(problemHint(shown)),
                actionLabel = when (shown) {
                    HostProblem.NoCompatibleMode -> stringResource(R.string.advanced_settings)
                    HostProblem.CameraBlocked -> openSettings
                    else -> null
                },
                onAction = when (shown) {
                    HostProblem.NoCompatibleMode -> { { showSettings = true } }
                    HostProblem.CameraBlocked -> { { SystemSettingsLauncher.openPrivacySettings(context) } }
                    else -> null
                },
            )
        }

        SetupStep.Streaming -> StepContent(
            title = stringResource(R.string.streaming_title),
            hint = stringResource(R.string.streaming_hint),
        )
    }
    // The same title the main pane shows, plus the actual negotiated mode when connected.
    val status = if (diagnostics.isEmpty()) content.title else "${content.title}\n$diagnostics"

    BackHandler(enabled = showSettings) { showSettings = false }
    if (showSettings) {
        AdvancedSettingsPanel(settings, status, viewModel::updateSettings) { showSettings = false }
        return
    }

    // Insets are applied once here; the panels below fill the remaining safe area.
    Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).safeDrawingPadding()) {
        OutlinedButton(
            onClick = { showSettings = true },
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(2.dp, Color.White),
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 8.dp).heightIn(min = 56.dp),
        ) {
            Text("${stringResource(R.string.advanced_settings)}  ›", textAlign = TextAlign.Center)
        }
        Box(Modifier.weight(1f)) {
            val action = content.onAction
            when {
                step == SetupStep.Streaming -> StreamingPanel(diagnostics)
                content.actionLabel != null && action != null -> ActionPanel(
                    step = step,
                    title = content.title,
                    hint = content.hint,
                    actionLabel = content.actionLabel,
                    onAction = action,
                )
                else -> WaitingPanel(step = step, title = content.title, hint = content.hint)
            }
        }
    }
}

private class StepContent(
    val title: String,
    val hint: String,
    val actionLabel: String? = null,
    val onAction: (() -> Unit)? = null,
)

@StringRes
private fun developerHintFor(family: OemFamily): Int = when (family) {
    OemFamily.Generic -> R.string.developer_hint_generic
    OemFamily.Samsung -> R.string.developer_hint_samsung
    OemFamily.Xiaomi -> R.string.developer_hint_xiaomi
    OemFamily.OnePlusOppo -> R.string.developer_hint_oneplus
}

/**
 * A confirmation buzz whenever the phone moves to a new step (not on first
 * launch). The brief connected state while the camera starts is skipped, so
 * a connection or settings restart buzzes once, when the picture is live.
 * Problems use the distinct rejection pattern.
 */
@Composable
private fun StepChangeHaptics(step: SetupStep) {
    val view = LocalView.current
    var lastStep by remember { mutableStateOf(step) }
    LaunchedEffect(step) {
        if (step != lastStep && step != SetupStep.HostConnected) {
            view.performHapticFeedback(
                if (step == SetupStep.Problem) HapticFeedbackConstants.REJECT else HapticFeedbackConstants.CONFIRM,
            )
        }
        lastStep = step
    }
}
