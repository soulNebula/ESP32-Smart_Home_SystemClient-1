package com.smarthome.ble.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Blue = Color(0xFF1565C0)
private val BlueLight = Color(0xFF5E92F3)
private val Amber = Color(0xFFFFB300)

private val LightColors = lightColorScheme(
    primary = Blue,
    secondary = Color(0xFF00897B),
    tertiary = Amber,
)

private val DarkColors = darkColorScheme(
    primary = BlueLight,
    secondary = Color(0xFF4DB6AC),
    tertiary = Amber,
)

@Composable
fun SmartHomeTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColors else LightColors,
        content = content,
    )
}
