package com.logx.tester.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

val PrimaryBlue = Color(0xFF1976D2)
val PrimaryBlueDark = Color(0xFF90CAF9)
val SecondaryTeal = Color(0xFF00796B)

private val DarkColorScheme = darkColorScheme(
    primary = PrimaryBlueDark,
    secondary = Color(0xFF80CBC4),
    tertiary = Color(0xFFFFB74D),
    surface = Color(0xFF1E1E1E),
    background = Color(0xFF121212)
)

private val LightColorScheme = lightColorScheme(
    primary = PrimaryBlue,
    secondary = SecondaryTeal,
    tertiary = Color(0xFFF57C00),
    surface = Color(0xFFF5F5F5),
    background = Color(0xFFFFFFFF)
)

@Composable
fun LogXTesterTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = true,
    content: @Composable () -> Unit
) {
    val colorScheme = when {
        dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }
        darkTheme -> DarkColorScheme
        else -> LightColorScheme
    }

    MaterialTheme(
        colorScheme = colorScheme,
        typography = Typography(),
        content = content
    )
}

