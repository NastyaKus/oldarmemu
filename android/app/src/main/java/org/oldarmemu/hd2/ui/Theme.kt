package org.oldarmemu.hd2.ui

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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import org.oldarmemu.hd2.Settings

/* fallback palette: HD2 graphite and Android green */
private val Light = lightColorScheme(
    primary = Color(0xFF006D3D), onPrimary = Color.White,
    primaryContainer = Color(0xFF97F7B8), onPrimaryContainer = Color(0xFF00210F),
    secondary = Color(0xFF4F6354), secondaryContainer = Color(0xFFD1E8D5),
    tertiary = Color(0xFF3B6470), tertiaryContainer = Color(0xFFBFE9F8),
    background = Color(0xFFF6FBF4), surface = Color(0xFFF6FBF4),
    surfaceContainer = Color(0xFFEBEFE9), surfaceContainerHigh = Color(0xFFE5EAE3),
)

private val Dark = darkColorScheme(
    primary = Color(0xFF7BDA9D), onPrimary = Color(0xFF00391D),
    primaryContainer = Color(0xFF00522C), onPrimaryContainer = Color(0xFF97F7B8),
    secondary = Color(0xFFB5CCB9), secondaryContainer = Color(0xFF374B3D),
    tertiary = Color(0xFFA3CDDB), tertiaryContainer = Color(0xFF214C58),
    background = Color(0xFF101411), surface = Color(0xFF101411),
    surfaceContainer = Color(0xFF1C211D), surfaceContainerHigh = Color(0xFF272B27),
)

@Composable
fun Hd2Theme(cfg: Settings, themeVersion: Int = 0, content: @Composable () -> Unit) {
    @Suppress("UNUSED_EXPRESSION") themeVersion
    val dark = when (cfg.theme) {
        Settings.THEME_DARK -> true
        Settings.THEME_LIGHT -> false
        else -> isSystemInDarkTheme()
    }
    val ctx = LocalContext.current
    val scheme = when {
        cfg.dynamicColor && Build.VERSION.SDK_INT >= 31 ->
            if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        dark -> Dark
        else -> Light
    }
    val t = Typography()
    MaterialTheme(
        colorScheme = scheme,
        typography = t.copy(
            displaySmall = t.displaySmall.copy(fontWeight = FontWeight.SemiBold),
            headlineMedium = t.headlineMedium.copy(fontWeight = FontWeight.SemiBold),
            titleLarge = t.titleLarge.copy(fontWeight = FontWeight.SemiBold, fontSize = 22.sp),
        ),
        content = content,
    )
}
