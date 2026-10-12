package com.okuflow.camera.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.okuflow.camera.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Original upstream license/NOTICE texts are read only when this screen is opened. */
@Composable
fun LicensesPanel(onBack: () -> Unit) {
    val context = LocalContext.current
    var notices by remember { mutableStateOf<String?>(null) }
    var failed by remember { mutableStateOf(false) }
    LaunchedEffect(context) {
        val result = withContext(Dispatchers.IO) {
            runCatching { context.assets.open("licenses/OPEN_SOURCE_NOTICES.txt").bufferedReader().use { it.readText() } }
        }
        notices = result.getOrNull()
        failed = result.isFailure
    }
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background,
        contentColor = MaterialTheme.colorScheme.onBackground) {
        Column(Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp)) {
            OutlinedButton(onClick = onBack, modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp)) {
                Text(stringResource(R.string.action_back_settings))
            }
            Text(stringResource(R.string.licenses_title), fontSize = 28.sp, lineHeight = 36.sp,
                modifier = Modifier.semantics { heading() })
            SelectionContainer {
                Text(notices ?: stringResource(if (failed) R.string.licenses_unavailable else R.string.licenses_loading),
                    fontSize = 18.sp, lineHeight = 26.sp)
            }
        }
    }
}
