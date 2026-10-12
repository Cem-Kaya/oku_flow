package com.okuflow.camera.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.okuflow.camera.R

/** A deliberate resume is the only way to release a saved pause. */
@Composable
fun StreamingIntentControl(paused: Boolean, onChange: () -> Unit) {
    OutlinedButton(onClick = onChange, shape = RoundedCornerShape(12.dp), border = BorderStroke(2.dp, Color.White),
        modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 4.dp).heightIn(min = 56.dp)) {
        Text(stringResource(if (paused) R.string.action_resume_streaming else R.string.action_pause_streaming),
            textAlign = TextAlign.Center)
    }
}
