package app.selfvoy.ui.theme

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat

/*
  Язык интерфейсов Selfvoy 2026 (brand/selfvoy-app.html, web/src/styles/global.css):
  системный шрифт, крупные жирные заголовки, акцент #3555F5, карточки на сером
  фоне, плавающие стеклянные элементы управления, светлая и тёмная темы.

  Токены — свойства с геттером поверх снапшот-состояния [SelfvoyPalette]: их можно
  читать откуда угодно (в том числе в drawBehind и в значениях по умолчанию), а
  смена темы перерисовывает всех, кто их прочитал. Старые имена (Accent,
  TextPrimary, BgDeep…) сохранены, чтобы экраны не переписывать ради цвета.
*/

/** Режим оформления: как в системе, всегда светлое или всегда тёмное. */
enum class ThemeMode(val key: String, val label: String) {
    SYSTEM("system", "Как в системе"),
    LIGHT("light", "Светлое"),
    DARK("dark", "Тёмное");

    companion object {
        fun of(key: String?): ThemeMode = entries.firstOrNull { it.key == key } ?: SYSTEM
    }
}

/** Активная палитра; [dark] выставляет [SelfvoyTheme]. */
object SelfvoyPalette {
    var dark by mutableStateOf(false)
    var mode by mutableStateOf(ThemeMode.SYSTEM)
}

private inline fun pick(light: Long, dark: Long): Color = Color(if (SelfvoyPalette.dark) dark else light)

// ---------------------------------------------------------------- основа (.app)

/** Фон экрана (--bg). */
val Bg: Color get() = pick(0xFFF2F2F7, 0xFF000000)
/** Карточка и группа списка (--card). */
val Card: Color get() = pick(0xFFFFFFFF, 0xFF1C1C1E)
/** Основной текст (--fg). */
val Fg: Color get() = pick(0xFF111114, 0xFFF5F5F7)
/** Вторичный текст (--sec). */
val Sec: Color get() = pick(0xFF6C6C74, 0xFF98989F)
/** Третичный: пунктир ленты, неактивное (--ter). */
val Ter: Color get() = pick(0xFFAEAEB4, 0xFF5C5C62)
/** Разделитель (--sep). */
val Hairline: Color get() = pick(0x213C3C43, 0x73545458)
/** Заливка полей, переключателей, кнопок (--fill). */
val FillSoft: Color get() = pick(0x1F787880, 0x3D787880)

val Tint: Color get() = pick(0xFF3555F5, 0xFF7D93FF)
val TintSoft: Color get() = pick(0xFFE9EDFF, 0x2E7D93FF)
val Green: Color get() = pick(0xFF1E8E50, 0xFF4CD483)
val GreenSoft: Color get() = pick(0xFFE3F5EA, 0x264CD483)
val Amber: Color get() = pick(0xFFA86400, 0xFFFFC24D)
val AmberSoft: Color get() = pick(0xFFFFF1D6, 0x26FFC24D)
val Red: Color get() = pick(0xFFD93A3F, 0xFFFF6B6F)
val RedSoft: Color get() = pick(0x1FD93A3F, 0x29FF6B6F)

/** Стекло плавающих элементов (--glass, --glass-line). */
val Glass: Color get() = pick(0xA8FFFFFF, 0x9E2C2C30)
val GlassLine: Color get() = pick(0xE6FFFFFF, 0x24FFFFFF)

/** Фирменный акцент героя «следующая встреча» — один в обеих темах. */
val BrandBlue = Color(0xFF3555F5)
/** Янтарь бренда: новое / требует внимания. */
val BrandAmber = Color(0xFFFFB224)

/** Цвета капсул ленты дня — по странице записи (--c1…--c5). */
val CapsuleColors = listOf(Color(0xFF3555F5), Color(0xFF22A06B), Color(0xFFF59E0B), Color(0xFFE8618C), Color(0xFF7C5CFC))

// ------------------------------------------------- прежние имена (совместимость)

val BgDeep: Color get() = Bg
val BgBase: Color get() = Bg
val BgRaised: Color get() = Card
val BgElevated: Color get() = Card
val BgPanel: Color get() = Card

val Accent: Color get() = Tint
val AccentSoft: Color get() = Tint
val AccentDeep: Color get() = Tint
val GradStart: Color get() = Tint
val GradMid: Color get() = Tint
val GradEnd: Color get() = Tint
val GlowViolet: Color get() = Color.Transparent
val GlowMagenta: Color get() = Color.Transparent
val GlowIndigo: Color get() = Color.Transparent
val Lilac: Color get() = Tint

val TextPrimary: Color get() = Fg
val TextSecondary: Color get() = Sec
val TextMuted: Color get() = pick(0xFF8E8E93, 0xFF7C7C82)

val Success: Color get() = Green
val Warning: Color get() = Amber
val Danger: Color get() = Red

val GlassFill: Color get() = FillSoft
val GlassBorder: Color get() = Hairline
val GlassHighlight: Color get() = FillSoft

val RadiusShell = 34.dp
val RadiusCard = 20.dp
val RadiusBubble = 18.dp
val RadiusField = 12.dp

private fun style(size: Double, weight: FontWeight, line: Double = size * 1.3, tracking: Double = 0.0) =
    TextStyle(fontFamily = FontFamily.Default, fontSize = size.sp, fontWeight = weight, lineHeight = line.sp,
        letterSpacing = tracking.sp)

/** Типографика 2026: системный шрифт, заголовки 34 extrabold с плотным трекингом. */
val SelfvoyTypography = Typography(
    displayLarge = style(46.0, FontWeight.ExtraBold, 48.0, -2.0),
    displayMedium = style(34.0, FontWeight.ExtraBold, 38.0, -1.2),
    headlineLarge = style(30.0, FontWeight.Bold, 34.0, -0.8),
    headlineMedium = style(24.0, FontWeight.Bold, 30.0, -0.5),
    headlineSmall = style(20.0, FontWeight.Bold, 26.0, -0.4),
    titleLarge = style(20.0, FontWeight.Bold, 26.0, -0.3),
    titleMedium = style(17.0, FontWeight.SemiBold, 22.0, -0.2),
    titleSmall = style(15.5, FontWeight.SemiBold, 20.0),
    bodyLarge = style(16.0, FontWeight.Normal),
    bodyMedium = style(15.0, FontWeight.Normal),
    bodySmall = style(13.0, FontWeight.Normal),
    labelLarge = style(16.0, FontWeight.SemiBold),
    labelMedium = style(13.5, FontWeight.SemiBold),
    labelSmall = style(11.0, FontWeight.SemiBold),
)

/**
 * Тема приложения. [mode] — выбор пользователя (Настройки → Оформление);
 * «как в системе» следует за системной тёмной темой.
 */
@Composable
fun SelfvoyTheme(mode: ThemeMode = SelfvoyPalette.mode, content: @Composable () -> Unit) {
    val systemDark = isSystemInDarkTheme()
    val dark = when (mode) {
        ThemeMode.SYSTEM -> systemDark
        ThemeMode.LIGHT -> false
        ThemeMode.DARK -> true
    }
    // Запись до чтения токенов в этой композиции: потомки сразу видят нужную палитру.
    if (SelfvoyPalette.dark != dark) SelfvoyPalette.dark = dark

    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as? Activity)?.window ?: return@SideEffect
            val background = (if (dark) Color.Black else Color(0xFFF2F2F7)).toArgb()
            @Suppress("DEPRECATION")
            window.statusBarColor = background
            @Suppress("DEPRECATION")
            window.navigationBarColor = background
            val controller = WindowCompat.getInsetsController(window, view)
            controller.isAppearanceLightStatusBars = !dark
            controller.isAppearanceLightNavigationBars = !dark
        }
    }

    val scheme = if (dark) {
        darkColorScheme(
            primary = Color(0xFF7D93FF), onPrimary = Color.White,
            primaryContainer = Color(0x2E7D93FF), onPrimaryContainer = Color(0xFF7D93FF),
            secondary = Color(0xFF7D93FF), onSecondary = Color.White,
            background = Color.Black, onBackground = Color(0xFFF5F5F7),
            surface = Color(0xFF1C1C1E), onSurface = Color(0xFFF5F5F7),
            surfaceVariant = Color(0xFF2C2C2E), onSurfaceVariant = Color(0xFF98989F),
            surfaceContainer = Color(0xFF1C1C1E), surfaceContainerHigh = Color(0xFF2C2C2E),
            surfaceContainerLow = Color(0xFF111112),
            error = Color(0xFFFF6B6F), onError = Color.White,
            outline = Color(0x73545458), outlineVariant = Color(0x73545458),
        )
    } else {
        lightColorScheme(
            primary = Color(0xFF3555F5), onPrimary = Color.White,
            primaryContainer = Color(0xFFE9EDFF), onPrimaryContainer = Color(0xFF3555F5),
            secondary = Color(0xFF3555F5), onSecondary = Color.White,
            background = Color(0xFFF2F2F7), onBackground = Color(0xFF111114),
            surface = Color.White, onSurface = Color(0xFF111114),
            surfaceVariant = Color(0xFFF2F2F7), onSurfaceVariant = Color(0xFF6C6C74),
            surfaceContainer = Color.White, surfaceContainerHigh = Color.White,
            surfaceContainerLow = Color(0xFFF2F2F7),
            error = Color(0xFFD93A3F), onError = Color.White,
            outline = Color(0x213C3C43), outlineVariant = Color(0x213C3C43),
        )
    }
    MaterialTheme(colorScheme = scheme, typography = SelfvoyTypography, content = content)
}

/** Статусный цвет по виду уведомления (токены green/amber/red). */
fun statusColor(kind: String): Color = when {
    kind.startsWith("twofactor") || kind == "login.new" -> Warning
    kind == "confirmation.requested" -> Danger
    kind == "task.due" || kind == "booking.confirmed" -> Success
    kind == "booking.payment" || kind == "package.payment" || kind == "package.low" -> Warning
    kind == "booking.cancelled" -> Danger
    else -> Accent
}
