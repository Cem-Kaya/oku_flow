package com.okuflow.camera.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.paneTitle
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import com.okuflow.camera.R
import com.okuflow.camera.link.HostProblem
import com.okuflow.camera.setup.SetupStep
import com.okuflow.camera.ui.theme.OkuFlowCameraTheme
import com.okuflow.camera.ui.theme.OkuFlowDimGray
import com.okuflow.camera.ui.theme.OkuFlowHintGray
import com.okuflow.camera.ui.theme.OkuFlowLiveGreen
import com.okuflow.camera.ui.theme.OkuFlowMagenta
import com.okuflow.camera.ui.theme.OkuFlowWarningAmber

/**
 * A whole-screen button: the entire panel is one large touch target with a
 * single action, which TalkBack reads as one element ("title, hint,
 * button, double-tap to <action>").
 */
@Composable
fun ActionPanel(
    step: SetupStep,
    title: String,
    hint: String,
    actionLabel: String,
    onAction: () -> Unit,
) {
    PanelScaffold(
        title = title,
        modifier = Modifier.clickable(
            onClickLabel = actionLabel,
            role = Role.Button,
            onClick = onAction,
        ),
        header = { StepHeader(step) },
        footer = { ActionPill(actionLabel) },
    ) {
        PanelText(title = title, hint = hint)
    }
}

/** A panel with nothing to tap: the phone is waiting for the user or the computer. */
@Composable
fun WaitingPanel(step: SetupStep, title: String, hint: String) {
    PanelScaffold(
        title = title,
        header = { StepHeader(step) },
        footer = {},
    ) {
        PanelText(title = title, hint = hint)
    }
}

/**
 * Shown while the phone sends the camera. Mostly black to save OLED power;
 * MainActivity keeps the screen on throughout the visible app.
 */
@Composable
fun StreamingPanel(details: String = "") {
    val title = stringResource(R.string.streaming_title)
    PanelScaffold(title = title, header = {}, footer = {}) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.Center,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Canvas(Modifier.size(20.dp)) { drawCircle(OkuFlowLiveGreen) }
            Text(
                text = title,
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onBackground,
                textAlign = TextAlign.Center,
                modifier = Modifier.weight(1f, fill = false).padding(start = 16.dp),
            )
        }
        Text(
            text = stringResource(R.string.streaming_hint) + if (details.isEmpty()) "" else "\n\n$details",
            style = MaterialTheme.typography.headlineSmall,
            color = OkuFlowHintGray,
            textAlign = TextAlign.Justify,
            modifier = Modifier.fillMaxWidth().padding(top = 16.dp),
        )
    }
}

/**
 * Header / body / footer spread over at least one screen height, scrolling
 * when a large system font scale makes the content taller than the screen.
 * [title] becomes the pane title so TalkBack announces every step change.
 * The caller applies the safe-drawing insets once, above the settings entry.
 */
@Composable
private fun PanelScaffold(
    title: String,
    modifier: Modifier = Modifier,
    header: @Composable () -> Unit,
    footer: @Composable () -> Unit,
    body: @Composable () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .semantics(mergeDescendants = true) { paneTitle = title }
            .then(modifier)
            .padding(horizontal = 28.dp, vertical = 24.dp),
    ) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            Column(
                // Scroll first, then the minimum height: the content is at
                // least one viewport tall and scrolls only when taller.
                modifier = Modifier
                    .fillMaxWidth()
                    .verticalScroll(rememberScrollState())
                    .heightIn(min = maxHeight),
                verticalArrangement = Arrangement.SpaceBetween,
            ) {
                header()
                Column(
                    modifier = Modifier.fillMaxWidth().padding(vertical = 32.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) { body() }
                footer()
            }
        }
    }
}

@Composable
private fun PanelText(title: String, hint: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.displaySmall,
        color = MaterialTheme.colorScheme.onBackground,
        textAlign = TextAlign.Center,
        modifier = Modifier.fillMaxWidth(),
    )
    Text(
        text = hint,
        style = MaterialTheme.typography.headlineSmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Justify,
        modifier = Modifier.fillMaxWidth().padding(top = 20.dp),
    )
}

@Composable
private fun StepHeader(step: SetupStep) {
    if (step == SetupStep.Problem) {
        Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { WarningBadge() }
        return
    }
    if (step == SetupStep.CheckUsbSetup || step == SetupStep.Paused) return
    val number = step.actionNumber
    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        if (number != null) {
            Text(
                text = stringResource(R.string.step_counter, number, SetupStep.ACTION_STEP_COUNT),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
        }
        ProgressDots(
            completed = (number ?: (SetupStep.ACTION_STEP_COUNT + 1)) - 1,
            modifier = Modifier.padding(top = 12.dp),
        )
    }
}

/** Decorative progress: filled magenta for finished steps, a white ring for the current one. */
@Composable
private fun ProgressDots(completed: Int, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        repeat(SetupStep.ACTION_STEP_COUNT) { index ->
            Canvas(Modifier.size(16.dp)) {
                when {
                    index < completed -> drawCircle(OkuFlowMagenta)
                    index == completed -> drawCircle(
                        color = Color.White,
                        radius = size.minDimension / 2 - 1.5.dp.toPx(),
                        center = Offset(size.width / 2, size.height / 2),
                        style = Stroke(width = 3.dp.toPx()),
                    )
                    else -> drawCircle(OkuFlowDimGray)
                }
            }
        }
    }
}

/** Decorative amber ring with an exclamation mark (about 13:1 on black); the title carries the meaning. */
@Composable
private fun WarningBadge() {
    Canvas(Modifier.size(56.dp)) {
        val stroke = 4.dp.toPx()
        drawCircle(OkuFlowWarningAmber, radius = size.minDimension / 2 - stroke / 2, style = Stroke(width = stroke))
        val x = size.width / 2
        drawLine(OkuFlowWarningAmber, Offset(x, size.height * 0.27f), Offset(x, size.height * 0.58f),
            strokeWidth = stroke * 1.4f, cap = StrokeCap.Round)
        drawCircle(OkuFlowWarningAmber, radius = stroke * 0.8f, center = Offset(x, size.height * 0.74f))
    }
}

@Composable
private fun ActionPill(label: String) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(min = 88.dp)
            .clip(RoundedCornerShape(44.dp))
            .background(MaterialTheme.colorScheme.primary)
            .padding(horizontal = 24.dp, vertical = 16.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.titleLarge,
            color = MaterialTheme.colorScheme.onPrimary,
            textAlign = TextAlign.Center,
        )
    }
}

@Preview(name = "Developer options", widthDp = 360, heightDp = 780)
@Composable
private fun ActionPanelPreview() {
    OkuFlowCameraTheme {
        ActionPanel(
            step = SetupStep.EnableDeveloperOptions,
            title = stringResource(R.string.developer_title),
            hint = stringResource(R.string.developer_hint_generic),
            actionLabel = stringResource(R.string.action_open_settings),
            onAction = {},
        )
    }
}

@Preview(name = "USB debugging", widthDp = 360, heightDp = 780)
@Composable
private fun UsbDebuggingPanelPreview() {
    OkuFlowCameraTheme {
        ActionPanel(
            step = SetupStep.EnableUsbDebugging,
            title = stringResource(R.string.usb_debugging_title),
            hint = stringResource(R.string.usb_debugging_hint),
            actionLabel = stringResource(R.string.action_open_settings),
            onAction = {},
        )
    }
}

@Preview(name = "Camera permission", widthDp = 360, heightDp = 780)
@Preview(name = "Camera permission, German 200%", widthDp = 320, heightDp = 640,
    locale = "de", fontScale = 2f)
@Composable
private fun CameraPermissionPanelPreview() {
    OkuFlowCameraTheme {
        ActionPanel(
            step = SetupStep.AllowCamera,
            title = stringResource(R.string.camera_title),
            hint = stringResource(R.string.camera_hint),
            actionLabel = stringResource(R.string.action_allow_camera),
            onAction = {},
        )
    }
}

@Preview(name = "Permission settings, Turkish", widthDp = 360, heightDp = 780, locale = "tr")
@Composable
private fun PermissionSettingsPanelPreview() {
    OkuFlowCameraTheme {
        ActionPanel(
            step = SetupStep.AllowCamera,
            title = stringResource(R.string.camera_title),
            hint = stringResource(R.string.camera_hint_settings),
            actionLabel = stringResource(R.string.action_open_settings),
            onAction = {},
        )
    }
}

@Preview(name = "Connect cable", widthDp = 360, heightDp = 780)
@Preview(name = "Connect cable, landscape", widthDp = 780, heightDp = 360)
@Composable
private fun ConnectUsbPanelPreview() {
    OkuFlowCameraTheme {
        WaitingPanel(
            step = SetupStep.ConnectUsb,
            title = stringResource(R.string.connect_usb_title),
            hint = stringResource(R.string.connect_usb_hint),
        )
    }
}

@Preview(name = "Waiting for OkuFlow", widthDp = 360, heightDp = 780)
@Composable
private fun WaitingPanelPreview() {
    OkuFlowCameraTheme {
        WaitingPanel(
            step = SetupStep.OpenOkuFlow,
            title = stringResource(R.string.open_okuflow_title),
            hint = stringResource(R.string.open_okuflow_hint),
        )
    }
}

@Preview(name = "Connected, camera starting", widthDp = 360, heightDp = 780)
@Composable
private fun HostConnectedPanelPreview() {
    OkuFlowCameraTheme {
        WaitingPanel(
            step = SetupStep.HostConnected,
            title = stringResource(R.string.connected_title),
            hint = stringResource(R.string.connected_hint),
        )
    }
}

@Preview(name = "Problem: camera busy", widthDp = 360, heightDp = 780)
@Preview(name = "Problem: camera busy, German 200%", widthDp = 320, heightDp = 640,
    locale = "de", fontScale = 2f)
@Composable
private fun ProblemPanelPreview() {
    OkuFlowCameraTheme {
        WaitingPanel(
            step = SetupStep.Problem,
            title = stringResource(problemTitle(HostProblem.CameraBusy)),
            hint = stringResource(problemHint(HostProblem.CameraBusy)),
        )
    }
}

@Preview(name = "Problem with action: mode", widthDp = 360, heightDp = 780, locale = "tr")
@Composable
private fun ProblemActionPanelPreview() {
    OkuFlowCameraTheme {
        ActionPanel(
            step = SetupStep.Problem,
            title = stringResource(problemTitle(HostProblem.NoCompatibleMode)),
            hint = stringResource(problemHint(HostProblem.NoCompatibleMode)),
            actionLabel = stringResource(R.string.advanced_settings),
            onAction = {},
        )
    }
}

@Preview(name = "Streaming", widthDp = 360, heightDp = 780)
@Composable
private fun StreamingPanelPreview() {
    OkuFlowCameraTheme { StreamingPanel() }
}
