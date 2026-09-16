package com.flavio.gpsposition.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val Scheme = darkColorScheme(
    primary = Accent,
    onPrimary = Bg,
    background = Bg,
    onBackground = TextMain,
    surface = Surface,
    onSurface = TextMain,
    secondary = Green,
    error = Red
)

@Composable
fun GpsPositionTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Scheme, content = content)
}
