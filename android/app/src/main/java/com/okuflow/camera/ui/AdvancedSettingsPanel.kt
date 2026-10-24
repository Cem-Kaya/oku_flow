package com.okuflow.camera.ui

import android.hardware.camera2.CameraCharacteristics
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.unit.LayoutDirection
import com.okuflow.camera.R
import com.okuflow.camera.camera.CameraCatalog
import com.okuflow.camera.camera.CameraChoice
import com.okuflow.camera.camera.StreamMode
import com.okuflow.camera.camera.everydayModes
import com.okuflow.camera.camera.withAvailableModes
import com.okuflow.camera.settings.AppLanguage
import com.okuflow.camera.settings.AppLanguageStore
import com.okuflow.camera.settings.CameraSettings
import com.okuflow.camera.ui.theme.OkuFlowCameraTheme
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

/**
 * Locally discovered size/rate pairs and labeled, scrollable image controls.
 * [status] is the main screen's current state (plus the negotiated mode while
 * connected), shown first on both levels so setting changes show their effect.
 */
@Composable
fun AdvancedSettingsPanel(settings: CameraSettings, status: String, onChange: ((CameraSettings) -> CameraSettings) -> Unit,
    paused: Boolean = false, onPauseChange: () -> Unit = {}, onBack: () -> Unit) {
    var showMoreOptions by rememberSaveable { mutableStateOf(false) }
    var showPrivacy by rememberSaveable { mutableStateOf(false) }
    var showLicenses by rememberSaveable { mutableStateOf(false) }
    val mainScroll = rememberScrollState()
    val moreScroll = rememberScrollState()
    if (showLicenses) {
        BackHandler { showLicenses = false }
        LicensesPanel { showLicenses = false }
        return
    }
    if (showPrivacy) {
        BackHandler { showPrivacy = false }
        PrivacyPanel { showPrivacy = false }
        return
    }
    BackHandler(enabled = showMoreOptions) { showMoreOptions = false }
    val context = LocalContext.current
    val backArrow = if (LocalLayoutDirection.current == LayoutDirection.Rtl) "›" else "‹"
    val forwardArrow = if (LocalLayoutDirection.current == LayoutDirection.Rtl) "‹" else "›"
    val catalog = remember(context) { CameraCatalog(context.applicationContext) }
    var cameras by remember { mutableStateOf(emptyList<CameraChoice>()) }
    LaunchedEffect(Unit) {
        cameras = withContext(Dispatchers.IO) { runCatching { catalog.cameras() }.getOrDefault(emptyList()) }
    }
    var modes by remember(settings.cameraId, settings.codec) { mutableStateOf(emptyList<StreamMode>()) }
    var loadingModes by remember(settings.cameraId, settings.codec) { mutableStateOf(true) }
    val currentOnChange by rememberUpdatedState(onChange)
    LaunchedEffect(settings.cameraId, settings.codec) {
        val completedModes = withContext(Dispatchers.IO) { runCatching { catalog.modes(settings) }.getOrDefault(emptyList()) }
        modes = completedModes
        loadingModes = false
        currentOnChange { current ->
            if (current.cameraId == settings.cameraId && current.codec == settings.codec)
                current.withAvailableModes(completedModes) else current
        }
    }
    val automatic = stringResource(R.string.setting_auto)
    val mainModes = remember(modes) { everydayModes(modes) }
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background,
        contentColor = MaterialTheme.colorScheme.onBackground) {
        Column(Modifier.fillMaxSize().safeDrawingPadding(), horizontalAlignment = Alignment.CenterHorizontally) {
            Column(Modifier.widthIn(max = 680.dp).fillMaxWidth().padding(horizontal = 20.dp)) {
                OutlinedButton(onClick = { if (showMoreOptions) showMoreOptions = false else onBack() },
                    shape = RoundedCornerShape(12.dp), border = BorderStroke(2.dp, Color.White),
                    modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp).heightIn(min = 56.dp)) {
                    Text("$backArrow  ${stringResource(if (showMoreOptions) R.string.action_back_settings else R.string.action_back)}",
                        fontSize = 20.sp, textAlign = TextAlign.Center)
                }
                Text(stringResource(if (showMoreOptions) R.string.settings_more else R.string.advanced_settings),
                    fontSize = 28.sp, lineHeight = 36.sp,
                    fontWeight = FontWeight.Bold, textAlign = TextAlign.Center,
                    modifier = Modifier.fillMaxWidth().padding(bottom = 16.dp).semantics { heading() })
            }
            StreamingIntentControl(paused, onPauseChange)
            SettingsDivider()
            Column(Modifier.weight(1f).widthIn(max = 680.dp).fillMaxWidth()
                .verticalScroll(if (showMoreOptions) moreScroll else mainScroll).padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(20.dp)) {
                SettingsSection(stringResource(R.string.setting_status)) {
                    Text(status, fontSize = 18.sp, lineHeight = 26.sp, textAlign = TextAlign.Center,
                        modifier = Modifier.fillMaxWidth().padding(16.dp))
                }
                if (showMoreOptions) {
                    SettingsHint(stringResource(R.string.setting_all_modes_hint))
                    SettingsSection(stringResource(R.string.settings_video)) {
                        ModeSetting(stringResource(R.string.setting_all_modes), settings, modes, loadingModes, onChange)
                        SettingsDivider()
                        ChoiceSetting(stringResource(R.string.setting_bitrate), settings.bitrateMbps,
                            listOf(0 to automatic, 10 to "10 Mbps", 20 to "20 Mbps", 40 to "40 Mbps", 80 to "80 Mbps")) {
                            onChange { current -> current.copy(bitrateMbps = it) }
                        }
                    }
                    OutlinedButton(onClick = { onChange { CameraSettings() } },
                        shape = RoundedCornerShape(16.dp), border = BorderStroke(2.dp, Color.White),
                        modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp)) {
                        Text(stringResource(R.string.settings_reset), fontSize = 20.sp, lineHeight = 28.sp, textAlign = TextAlign.Center)
                    }
                } else {
                    SettingsHint(stringResource(R.string.advanced_hint))
                    SettingsSection(stringResource(R.string.settings_language)) {
                        LanguageSetting()
                    }
                    SettingsSection(stringResource(R.string.settings_video)) {
                        ChoiceSetting(stringResource(R.string.setting_camera), settings.cameraId,
                            listOf("" to automatic) + cameras.map { camera ->
                                val label = when (camera.role) {
                                    "auto" -> R.string.setting_auto
                                    "ultrawide" -> R.string.camera_ultrawide
                                    "main" -> R.string.camera_main
                                    "telephoto" -> R.string.camera_telephoto
                                    "front" -> R.string.camera_front
                                    else -> R.string.camera_back
                                }
                                camera.id to "${stringResource(label)} · ${camera.id}"
                            }) { onChange { current -> current.copy(cameraId = it) } }
                        SettingsDivider()
                        ModeSetting(stringResource(R.string.setting_mode), settings, mainModes, loadingModes, onChange)
                        SettingsHint(stringResource(R.string.setting_main_modes_hint),
                            Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
                        if (!loadingModes && settings.hasExplicitMode &&
                            (minOf(settings.modeWidth, settings.modeHeight) < 720 || settings.fps < 24)) {
                            SettingsHint(stringResource(R.string.setting_extra_mode_selected),
                                Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
                        }
                        SettingsDivider()
                        ChoiceSetting(stringResource(R.string.setting_codec), settings.codec,
                            listOf("avc" to "H.264", "hevc" to "HEVC / H.265")) { onChange { current -> current.copy(codec = it) } }
                    }
                    OutlinedButton(onClick = { showMoreOptions = true }, shape = RoundedCornerShape(12.dp),
                        border = BorderStroke(2.dp, Color.White), modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp)) {
                        Text("${stringResource(R.string.settings_more)}  $forwardArrow", fontSize = 20.sp, lineHeight = 28.sp,
                            textAlign = TextAlign.Center)
                    }
                    SettingsSection(stringResource(R.string.settings_image)) {
                        ChoiceSetting(stringResource(R.string.setting_stabilization), settings.stabilization,
                            listOf("off" to stringResource(R.string.setting_off), "optical" to stringResource(R.string.setting_optical),
                                "electronic" to stringResource(R.string.setting_electronic))) { onChange { current -> current.copy(stabilization = it) } }
                        SettingsHint(stringResource(R.string.stabilization_hint), Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
                        SettingsDivider()
                        ToggleSetting(stringResource(R.string.setting_autofocus), settings.autofocus,
                            stringResource(R.string.focus_hint)) { onChange { current -> current.copy(autofocus = it) } }
                        SettingsDivider()
                        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text(stringResource(R.string.setting_torch), fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.Medium)
                            SettingsHint(stringResource(R.string.torch_host_hint))
                        }
                        SettingsDivider()
                        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            val exposureLabel = stringResource(R.string.setting_exposure, settings.exposure)
                            Text(exposureLabel, fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.Medium)
                            SettingsHint(stringResource(R.string.exposure_hint))
                            Slider(value = settings.exposure.toFloat(),
                                onValueChange = { onChange { current -> current.copy(exposure = it.roundToInt()) } },
                                valueRange = -12f..12f, steps = 23,
                                modifier = Modifier.fillMaxWidth().semantics {
                                    contentDescription = exposureLabel
                                    stateDescription = exposureLabel
                                })
                        }
                    }
                    OptionalNotificationControl()
                    OutlinedButton(onClick = { showPrivacy = true }, shape = RoundedCornerShape(12.dp),
                        border = BorderStroke(2.dp, Color.White), modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp)) {
                        Text(stringResource(R.string.privacy_title), fontSize = 20.sp, lineHeight = 28.sp,
                            textAlign = TextAlign.Center)
                    }
                    OutlinedButton(onClick = { showLicenses = true }, shape = RoundedCornerShape(12.dp),
                        border = BorderStroke(2.dp, Color.White), modifier = Modifier.fillMaxWidth().heightIn(min = 64.dp)) {
                        Text(stringResource(R.string.licenses_title), fontSize = 20.sp, lineHeight = 28.sp,
                            textAlign = TextAlign.Center)
                    }
                }
            }
        }
    }
}

/**
 * App language: System default follows later phone language changes; an
 * explicit choice persists (Android 13+ also shows it in system app settings).
 */
@Composable
private fun LanguageSetting() {
    val context = LocalContext.current
    val activity = LocalActivity.current
    // Changing the language recreates the activity, which re-reads both values.
    val selected = remember { AppLanguageStore.selected(context) }
    val systemLanguage = remember { AppLanguageStore.systemLanguage() }
    ChoiceSetting(stringResource(R.string.setting_language), selected?.tag ?: "",
        listOf("" to stringResource(R.string.language_system_default, systemLanguage.nativeName)) +
            AppLanguage.entries.map { it.tag to it.nativeName }) { tag ->
        if (AppLanguageStore.select(context, AppLanguage.fromTag(tag))) activity?.recreate()
    }
    SettingsHint(stringResource(R.string.language_hint), Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
}

/** A shared selector; a selection made in More options stays visible even when the main list hides it. */
@Composable
private fun ModeSetting(label: String, settings: CameraSettings, modes: List<StreamMode>, loading: Boolean,
    onChange: ((CameraSettings) -> CameraSettings) -> Unit) {
    val selected = if (settings.hasExplicitMode) "${settings.modeWidth}x${settings.modeHeight}@${settings.fps}" else ""
    val automatic = stringResource(R.string.setting_auto)
    ChoiceSetting(label, selected, listOf("" to automatic) + modes.map { mode ->
        mode.key to stringResource(R.string.setting_mode_format, mode.width, mode.height, mode.fps)
    }, enabled = !loading, valueLabel = when {
        loading -> stringResource(R.string.setting_modes_loading)
        settings.hasExplicitMode -> stringResource(R.string.setting_mode_format, settings.modeWidth, settings.modeHeight, settings.fps)
        else -> automatic
    }) { key ->
        val mode = modes.firstOrNull { it.key == key }
        onChange { current -> current.copy(resolution = 0, modeWidth = mode?.width ?: 0, modeHeight = mode?.height ?: 0,
            fps = mode?.fps ?: 60) }
    }
    if (!loading && modes.isEmpty()) SettingsHint(stringResource(R.string.setting_modes_unavailable),
        Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
}

@Composable
private fun SettingsSection(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(title, fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.Bold, textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth().semantics { heading() })
        Surface(color = Color(0xFF161616), contentColor = MaterialTheme.colorScheme.onSurface,
            shape = RoundedCornerShape(16.dp), border = BorderStroke(1.dp, Color(0xFF424242))) {
            Column(Modifier.fillMaxWidth(), content = content)
        }
    }
}

@Composable
private fun SettingsDivider() = HorizontalDivider(color = Color(0xFF424242))

@Composable
private fun SettingsHint(text: String, modifier: Modifier = Modifier) {
    Text(text, fontSize = 18.sp, lineHeight = 26.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Justify, modifier = modifier.fillMaxWidth())
}

@Composable
private fun <T> ChoiceSetting(label: String, value: T, options: List<Pair<T, String>>,
    enabled: Boolean = true, valueLabel: String? = null, onChange: (T) -> Unit) {
    var expanded by remember(enabled, options) { mutableStateOf(false) }
    val menuScroll = rememberScrollState()
    val itemHeights = remember(options) { mutableStateMapOf<Int, Int>() }
    val selectedIndex = options.indexOfFirst { it.first == value }
    val dividerHeight = with(LocalDensity.current) { 1.dp.roundToPx() }
    val selectedOffset = if (selectedIndex >= 0 && (0 until selectedIndex).all { it in itemHeights })
        (0 until selectedIndex).sumOf { itemHeights.getValue(it) + dividerHeight } else null
    LaunchedEffect(expanded, value, selectedOffset) {
        if (expanded && selectedOffset != null) menuScroll.scrollTo(selectedOffset)
    }
    BoxWithConstraints(Modifier.fillMaxWidth().padding(12.dp)) {
        val choiceShape = RoundedCornerShape(12.dp)
        Row(Modifier.fillMaxWidth().semantics(mergeDescendants = true) {
                stateDescription = valueLabel ?: options.firstOrNull { it.first == value }?.second ?: value.toString()
            }.border(2.dp, if (enabled) Color.White else Color.Gray, choiceShape)
            .clip(choiceShape).clickable(enabled = enabled, role = Role.Button, onClick = { expanded = true })
            .heightIn(min = 96.dp).padding(horizontal = 20.dp, vertical = 18.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(label, fontSize = 18.sp, lineHeight = 24.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(valueLabel ?: options.firstOrNull { it.first == value }?.second ?: value.toString(),
                    fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.Medium)
            }
            Text("▾", fontSize = 24.sp, modifier = Modifier.clearAndSetSemantics { })
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false },
            modifier = Modifier.width(maxWidth).heightIn(max = 440.dp), shape = choiceShape,
            scrollState = menuScroll, containerColor = Color(0xFF161616), tonalElevation = 0.dp,
            border = BorderStroke(2.dp, Color.White)) {
            options.forEachIndexed { index, option ->
                val isSelected = option.first == value
                val foreground = if (isSelected) Color.Black else Color.White
                DropdownMenuItem(text = { Text(option.second, fontSize = 22.sp, lineHeight = 28.sp, color = foreground,
                    fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal) },
                    trailingIcon = if (isSelected) { { Text("✓", color = foreground,
                        modifier = Modifier.clearAndSetSemantics { }) } } else null,
                    modifier = Modifier.semantics { selected = isSelected }
                        .onSizeChanged { itemHeights[index] = it.height }
                        .heightIn(min = 76.dp).background(if (isSelected) Color.White else Color.Transparent)
                        .padding(vertical = 8.dp),
                    onClick = { expanded = false; onChange(option.first) })
                if (index < options.lastIndex) HorizontalDivider(color = Color(0xFF888888))
            }
        }
    }
}

@Composable
private fun ToggleSetting(label: String, enabled: Boolean, hint: String? = null, onChange: (Boolean) -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().toggleable(enabled, role = Role.Switch, onValueChange = onChange)
            .heightIn(min = 72.dp).padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(label, fontSize = 22.sp, lineHeight = 28.sp, fontWeight = FontWeight.Medium, modifier = Modifier.weight(1f))
            Switch(checked = enabled, onCheckedChange = null)
        }
        if (hint != null) SettingsHint(hint, Modifier.padding(start = 16.dp, end = 16.dp, bottom = 16.dp))
    }
}

@Preview(name = "Advanced settings", widthDp = 393, heightDp = 851, showBackground = true, backgroundColor = 0xFF000000)
@Preview(name = "Advanced settings large German", widthDp = 393, heightDp = 851, fontScale = 2f, locale = "de", showBackground = true, backgroundColor = 0xFF000000)
@Composable
private fun AdvancedSettingsPreview() {
    OkuFlowCameraTheme {
        AdvancedSettingsPanel(CameraSettings(resolution = 1080), "Streaming to OkuFlow\n1920 × 1080 · 60 FPS · H.264", {}) {}
    }
}
