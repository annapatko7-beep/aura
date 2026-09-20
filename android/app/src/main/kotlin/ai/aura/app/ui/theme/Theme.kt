package ai.aura.app.ui.theme

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

// Палитра «Dark Glassmorphism» — те же токены, что в client/qml/theme/AuraTheme.qml
// и docs/DESIGN_SYSTEM.md (full_mix): графит + полупрозрачные пузыри, акцент
// #7C6AFF.

val BgDeep = Color(0xFF0D0D0F)
val BgBase = Color(0xFF16161A)
val BgRaised = Color(0xFF1E1E24)
val BgElevated = Color(0xFF26262E)

val Accent = Color(0xFF7C6AFF)
val AccentSoft = Color(0xFFA78BFA)
val AccentDeep = Color(0xFF5B4DD6)

val TextPrimary = Color(0xFFF0F0F4)
val TextSecondary = Color(0xFF9A9AA4)
val TextMuted = Color(0xFF5C5C66)

val Success = Color(0xFF4ADE80)
val Warning = Color(0xFFFBBF24)
val Danger = Color(0xFFF87171)

// Стекло: fill 4% белого, рамка 8% белого ( AuraTheme.glassFill/glassBorder).
val GlassFill = Color(0x0AFFFFFF)
val GlassBorder = Color(0x14FFFFFF)
val GlassHighlight = Color(0x24FFFFFF)

val RadiusCard = 22.dp
val RadiusField = 14.dp

private val AuraColorScheme = darkColorScheme(
    primary = Accent,
    onPrimary = Color.White,
    primaryContainer = AccentDeep,
    onPrimaryContainer = Color.White,
    secondary = AccentSoft,
    onSecondary = Color.Black,
    background = BgDeep,
    onBackground = TextPrimary,
    surface = BgBase,
    onSurface = TextPrimary,
    surfaceVariant = BgRaised,
    onSurfaceVariant = TextSecondary,
    error = Danger,
    onError = Color.White,
    outline = GlassBorder,
)

@Composable
fun AuraTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = AuraColorScheme, content = content)
}

/** Стеклянная карточка (аналог GlassPanel.qml): fill + тонкая рамка + радиус 22. */
@Composable
fun GlassPanel(modifier: Modifier = Modifier, content: @Composable BoxScope.() -> Unit) {
    Box(
        modifier = modifier
            .background(
                brush = Brush.verticalGradient(listOf(GlassHighlight, GlassFill)),
                shape = RoundedCornerShape(RadiusCard),
            )
            .border(1.dp, GlassBorder, RoundedCornerShape(RadiusCard))
            .padding(16.dp),
        content = content,
    )
}

/** Статусный цвет по смыслу (токены success/warning/danger). */
fun statusColor(kind: String): Color = when {
    kind.startsWith("twofactor") || kind == "login.new" -> Warning
    kind == "confirmation.requested" -> Danger
    kind == "task.due" -> Success
    else -> AccentSoft
}

@Composable
fun ScreenBackground(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.background(Brush.verticalGradient(listOf(BgBase, BgDeep))),
        color = Color.Transparent,
    ) { content() }
}
