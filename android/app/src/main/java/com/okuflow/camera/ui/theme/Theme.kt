package com.okuflow.camera.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

// OkuFlow brand: white magnifier, black lens, magenta sparkle. Magenta on
// black is only about 4.5:1, so it is used for decoration only; all text and
// the action button are white/black (21:1) for low-vision users.
val OkuFlowBlack = Color(0xFF000000)
val OkuFlowWhite = Color(0xFFFFFFFF)
val OkuFlowMagenta = Color(0xFFC724C4)
val OkuFlowHintGray = Color(0xFFD9D9D9)
val OkuFlowDimGray = Color(0xFF5C5C5C)
val OkuFlowLiveGreen = Color(0xFF3DDC84)
// Problem marker only; titles stay white.
val OkuFlowWarningAmber = Color(0xFFFFC107)

private val OkuFlowColors = darkColorScheme(
    primary = OkuFlowWhite,
    onPrimary = OkuFlowBlack,
    secondary = OkuFlowMagenta,
    onSecondary = OkuFlowWhite,
    background = OkuFlowBlack,
    onBackground = OkuFlowWhite,
    surface = OkuFlowBlack,
    onSurface = OkuFlowWhite,
    onSurfaceVariant = OkuFlowHintGray,
    primaryContainer = Color(0xFF252525),
    onPrimaryContainer = OkuFlowWhite,
    secondaryContainer = Color(0xFF444444),
    onSecondaryContainer = OkuFlowWhite,
    surfaceContainer = Color(0xFF1F1F1F),
    surfaceContainerHigh = Color(0xFF252525),
    surfaceContainerHighest = Color(0xFF303030),
    outline = Color(0xFF878787),
    outlineVariant = Color(0xFF424242),
)

// Sizes are in sp so the system font scale still applies on top; every
// panel scrolls, so the largest font scale never clips text.
private val OkuFlowTypography = Typography(
    displaySmall = TextStyle(fontSize = 40.sp, lineHeight = 48.sp, fontWeight = FontWeight.Bold),
    headlineSmall = TextStyle(fontSize = 26.sp, lineHeight = 34.sp, fontWeight = FontWeight.Normal),
    titleLarge = TextStyle(fontSize = 28.sp, lineHeight = 34.sp, fontWeight = FontWeight.Bold),
    labelLarge = TextStyle(fontSize = 20.sp, lineHeight = 26.sp, fontWeight = FontWeight.Medium),
)

@Composable
fun OkuFlowCameraTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = OkuFlowColors,
        typography = OkuFlowTypography,
    ) {
        // MaterialTheme supplies a palette, but bare Text outside a Surface
        // otherwise inherits the platform's black LocalContentColor.
        CompositionLocalProvider(LocalContentColor provides OkuFlowColors.onBackground, content = content)
    }
}
