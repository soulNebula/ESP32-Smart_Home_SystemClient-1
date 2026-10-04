/*
 * 模块：
 *   配色。定两套颜色方案，手机开深色模式就用深色那套，否则用浅色那套。
 *   界面 MainActivity.kt 里所有文字、按钮、卡片的颜色都从这里取，
 *   自己不碰数据，也不管蓝牙。
 *
 * 功能：
 *   定浅色方案
 *   定深色方案
 *   跟着系统切换
 */
package com.smarthome.ble.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// 功能：主色和强调色
private val Blue = Color(0xFF1565C0)
private val BlueLight = Color(0xFF5E92F3)
private val Amber = Color(0xFFFFB300)

// 功能：白天用的三色
private val LightColors = lightColorScheme(
    primary = Blue,
    secondary = Color(0xFF00897B),
    tertiary = Amber,
)

// 功能：晚上用的三色
private val DarkColors = darkColorScheme(
    primary = BlueLight,
    secondary = Color(0xFF4DB6AC),
    tertiary = Amber,
)

/* 功能：给界面套上配色 */
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
