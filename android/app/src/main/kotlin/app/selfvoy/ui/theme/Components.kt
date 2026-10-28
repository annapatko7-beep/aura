package app.selfvoy.ui.theme

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AccessTime
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Block
import androidx.compose.material.icons.outlined.CalendarMonth
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.NotificationsNone
import androidx.compose.material.icons.outlined.PanTool
import androidx.compose.material.icons.outlined.Remove
import androidx.compose.material.icons.automirrored.outlined.Undo
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs

// Общие элементы языка 2026: карточки на сером фоне, сгруппированные списки,
// плавающее стекло, статусы «значок + слово», окна снизу с большими скруглениями.

/** Фон приложения — сплошной (--bg); стекло и карточки лежат поверх. */
@Composable
fun SelfvoyBackground(modifier: Modifier = Modifier, content: @Composable BoxScope.() -> Unit) {
    Box(modifier = modifier.fillMaxSize().background(Bg), content = content)
}

/** Кант стекла (совместимость со старыми экранами). */
val GlassEdge: Brush get() = SolidColor(GlassLine)

/**
 * Карточка (--card, радиус 20). [tint] ≠ белого — лёгкая цветная подложка поверх
 * карточки (например, акцентный блок); [glow] в языке 2026 не используется.
 */
@Composable
fun GlassCard(
    modifier: Modifier = Modifier,
    radius: Dp = RadiusCard,
    glow: Boolean = false,
    tint: Color = Color.White,
    fill: Float = 0.055f,
    contentPadding: PaddingValues = PaddingValues(16.dp),
    onClick: (() -> Unit)? = null,
    content: @Composable BoxScope.() -> Unit,
) {
    val shape = RoundedCornerShape(radius)
    Box(
        modifier = modifier
            .clip(shape)
            .background(Card)
            .then(if (tint != Color.White) Modifier.background(tint.copy(alpha = fill.coerceIn(0f, 0.25f))) else Modifier)
            .then(if (glow) Modifier.border(1.dp, Tint.copy(alpha = 0.35f), shape) else Modifier)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(contentPadding),
        content = content,
    )
}

/** Мягкое свечение вокруг элемента (совместимость; в 2026 — едва заметная тень). */
fun Modifier.outerGlow(radius: Dp, color: Color = Accent, spread: Dp = 22.dp): Modifier = drawBehind {
    val steps = 6
    val step = spread.toPx() / steps / 2
    for (i in 1..steps) {
        val d = step * i
        drawRoundRect(
            color = color.copy(alpha = 0.05f * (1f - i / (steps + 1f))),
            topLeft = Offset(-d, -d),
            size = androidx.compose.ui.geometry.Size(size.width + 2 * d, size.height + 2 * d),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(radius.toPx() + d),
            style = Stroke(width = step),
        )
    }
}

/** Совместимость со старыми экранами. */
@Composable
fun GlassPanel(modifier: Modifier = Modifier, content: @Composable BoxScope.() -> Unit) =
    GlassCard(modifier = modifier, contentPadding = PaddingValues(16.dp), content = content)

/**
 * Плавающее стекло (таб-бар, «+», поле ввода): полупрозрачная заливка, светлый кант
 * и мягкая тень. Текст на стекле не кладём — только значки и подписи кнопок.
 */
fun Modifier.glass(shape: androidx.compose.ui.graphics.Shape): Modifier = this
    .shadow(18.dp, shape, ambientColor = Color(0x38141828), spotColor = Color(0x38141828))
    .clip(shape)
    .background(Card.copy(alpha = 0.72f))
    .background(Glass)
    .border(1.dp, GlassLine, shape)

/** Основная кнопка: сплошной акцент (--tint), пилюля. Имя — историческое. */
@Composable
fun GradientButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    busy: Boolean = false,
    height: Dp = 50.dp,
) {
    PillButton(text, onClick, modifier, icon, enabled, busy, height, background = Tint, foreground = Color.White)
}

/** Кнопка «мягкая»: tint-soft фон и акцентный текст (.pbtn.soft). */
@Composable
fun SoftButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    busy: Boolean = false,
    height: Dp = 44.dp,
) {
    PillButton(text, onClick, modifier, icon, enabled, busy, height, background = TintSoft, foreground = Tint)
}

@Composable
private fun PillButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier,
    icon: ImageVector?,
    enabled: Boolean,
    busy: Boolean,
    height: Dp,
    background: Color,
    foreground: Color,
) {
    val shape = RoundedCornerShape(50)
    Row(
        modifier = modifier
            .height(height)
            .alpha(if (enabled) 1f else 0.45f)
            .clip(shape)
            .background(background)
            .clickable(enabled = enabled && !busy, onClick = onClick)
            .padding(horizontal = 20.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (busy) {
            CircularProgressIndicator(Modifier.size(18.dp), color = foreground, strokeWidth = 2.dp)
            Spacer(Modifier.width(10.dp))
        } else if (icon != null) {
            Icon(icon, contentDescription = null, tint = foreground, modifier = Modifier.size(18.dp))
            if (text.isNotEmpty()) Spacer(Modifier.width(8.dp))
        }
        if (text.isNotEmpty()) {
            Text(text, color = foreground, style = MaterialTheme.typography.labelLarge, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
        }
    }
}

/** Вторичная кнопка-пилюля: заливка --fill; danger — красная; quiet — без заливки. */
@Composable
fun GlassButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    icon: ImageVector? = null,
    enabled: Boolean = true,
    danger: Boolean = false,
    quiet: Boolean = false,
    height: Dp = 46.dp,
) {
    val shape = RoundedCornerShape(50)
    val fg = when {
        danger -> Red
        quiet -> Tint
        else -> Fg
    }
    Row(
        modifier = modifier
            .height(height)
            .alpha(if (enabled) 1f else 0.45f)
            .clip(shape)
            .background(
                when {
                    danger -> RedSoft
                    quiet -> Color.Transparent
                    else -> FillSoft
                },
            )
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = if (text.isEmpty()) 12.dp else 16.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (icon != null) {
            Icon(icon, contentDescription = text.ifEmpty { null }, tint = fg, modifier = Modifier.size(18.dp))
            if (text.isNotEmpty()) Spacer(Modifier.width(7.dp))
        }
        if (text.isNotEmpty()) Text(text, color = fg, style = MaterialTheme.typography.labelMedium, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
    }
}

/** Круглая иконка-кнопка (микрофон, отправка, «назад»). */
@Composable
fun RoundIconButton(
    icon: ImageVector,
    contentDescription: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    accent: Boolean = false,
    size: Dp = 44.dp,
    enabled: Boolean = true,
) {
    Box(
        modifier = modifier
            .size(size)
            .alpha(if (enabled) 1f else 0.45f)
            .clip(CircleShape)
            .background(if (accent) Tint else FillSoft)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = contentDescription,
            tint = if (accent) Color.White else Fg, modifier = Modifier.size(size * 0.46f))
    }
}

/** Пилюля-чип: фильтры и варианты. Активный — tint-soft с акцентным текстом. */
@Composable
fun SelfvoyChip(text: String, active: Boolean = false, enabled: Boolean = true, onClick: () -> Unit) {
    val shape = RoundedCornerShape(50)
    Box(
        modifier = Modifier
            .height(34.dp)
            .alpha(if (enabled) 1f else 0.55f)
            .clip(shape)
            .background(if (active) TintSoft else FillSoft)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, style = MaterialTheme.typography.labelMedium, color = if (active) Tint else Fg, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
    }
}

/** Поле ввода: заливка --fill, акцентная рамка в фокусе, подпись сверху. */
@Composable
fun SelfvoyField(
    value: String,
    onValueChange: (String) -> Unit,
    placeholder: String,
    modifier: Modifier = Modifier,
    label: String = "",
    trailing: ImageVector? = null,
    onTrailingClick: (() -> Unit)? = null,
    password: Boolean = false,
    filled: Boolean = false,
    readOnly: Boolean = false,
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    onDone: (() -> Unit)? = null,
    /** >1 — многострочное поле (например, «О себе»): растёт до maxLines строк. */
    maxLines: Int = 1,
) {
    val interaction = remember { MutableInteractionSource() }
    val focused by interaction.collectIsFocusedAsState()
    val shape = RoundedCornerShape(RadiusField)
    Column(modifier = modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (label.isNotEmpty()) Text(label, style = MaterialTheme.typography.labelMedium, color = Sec)
        BasicTextField(
            value = value,
            onValueChange = onValueChange,
            readOnly = readOnly,
            singleLine = maxLines <= 1,
            maxLines = maxLines.coerceAtLeast(1),
            interactionSource = interaction,
            textStyle = TextStyle(fontFamily = Manrope, fontSize = 16.sp, color = Fg),
            cursorBrush = SolidColor(Tint),
            visualTransformation = if (password) PasswordVisualTransformation() else VisualTransformation.None,
            keyboardOptions = keyboardOptions,
            keyboardActions = KeyboardActions(onDone = { onDone?.invoke() }, onGo = { onDone?.invoke() },
                onSend = { onDone?.invoke() }),
            decorationBox = { inner ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .then(if (maxLines <= 1) Modifier.height(46.dp) else Modifier.heightIn(min = 46.dp))
                        .clip(shape)
                        .background(if (filled) Card else FillSoft)
                        .border(1.dp, if (focused) Tint.copy(alpha = 0.8f) else Color.Transparent, shape)
                        .padding(start = 14.dp, end = 12.dp)
                        .then(if (maxLines <= 1) Modifier else Modifier.padding(vertical = 12.dp)),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Box(Modifier.weight(1f)) {
                        if (value.isEmpty()) Text(placeholder, color = TextMuted, style = MaterialTheme.typography.bodyLarge, maxLines = 1,
                            overflow = TextOverflow.Ellipsis)
                        inner()
                    }
                    if (trailing != null) {
                        Icon(
                            trailing, contentDescription = null, tint = Sec,
                            modifier = Modifier
                                .size(20.dp)
                                .then(if (onTrailingClick != null) Modifier.clickable(onClick = onTrailingClick) else Modifier),
                        )
                    }
                }
            },
        )
    }
}

/** Аватар: пастельный круг из имени и инициалы (как .av в макете). */
@Composable
fun SelfvoyAvatar(name: String, size: Dp = 44.dp, ring: Boolean = false, online: Boolean = false) {
    val hue = remember(name) { name.fold(0) { acc, c -> (acc * 31 + c.code) % 360 }.let { abs(it).toFloat() } }
    val initials = remember(name) {
        val parts = name.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
        when {
            parts.isEmpty() -> "?"
            parts.size == 1 -> parts[0].take(1).uppercase()
            else -> (parts[0].take(1) + parts[1].take(1)).uppercase()
        }
    }
    Box(contentAlignment = Alignment.Center) {
        if (ring) Box(Modifier.size(size + 8.dp).border(2.dp, Tint.copy(alpha = 0.35f), CircleShape))
        Box(
            modifier = Modifier
                .size(size)
                .clip(CircleShape)
                .background(Brush.linearGradient(listOf(Color.hsl(hue, 0.9f, 0.86f), Color.hsl((hue + 40) % 360, 0.8f, 0.82f)))),
            contentAlignment = Alignment.Center,
        ) {
            Text(initials, color = Color(0xFF111114), fontWeight = FontWeight.Bold, fontSize = (size.value * 0.34f).sp)
        }
        if (online) {
            Box(
                Modifier
                    .align(Alignment.BottomEnd)
                    .size(size * 0.26f)
                    .clip(CircleShape)
                    .background(Green)
                    .border(2.dp, Bg, CircleShape),
            )
        }
    }
}

/**
 * Знак Selfvoy: S-линия между двумя точками (brand/selfvoy-app.html, symbol #mk):
 * линия — акцент, верхняя точка — цвет текста, нижняя — янтарь.
 */
@Composable
fun SelfvoyEmblem(size: Dp = 72.dp, modifier: Modifier = Modifier, line: Color = BrandBlue, dotA: Color = Fg, dotB: Color = BrandAmber) {
    Canvas(modifier.size(size)) {
        val s = this.size.minDimension / 100f
        val path = Path().apply {
            moveTo(76 * s, 28 * s)
            cubicTo(38 * s, 24 * s, 28 * s, 44 * s, 50 * s, 50 * s)
            cubicTo(72 * s, 56 * s, 62 * s, 76 * s, 24 * s, 72 * s)
        }
        drawPath(path, line, style = Stroke(width = 11 * s, cap = StrokeCap.Round))
        drawCircle(dotA, radius = 10 * s, center = Offset(76 * s, 28 * s))
        drawCircle(dotB, radius = 10 * s, center = Offset(24 * s, 72 * s))
    }
}

/** Значок ассистента (шапка «Ассистента»): знак Selfvoy на круге. */
@Composable
fun SelfvoyOrb(size: Dp = 56.dp) {
    Box(Modifier.size(size).clip(CircleShape).background(Fg), contentAlignment = Alignment.Center) {
        SelfvoyEmblem(size = size * 0.7f, line = Bg, dotA = Bg, dotB = BrandAmber)
    }
}

/** Заголовок экрана (34 extrabold) с действиями справа. */
@Composable
fun ScreenHeader(title: String, modifier: Modifier = Modifier, actions: @Composable RowScope.() -> Unit = {}) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(title, style = MaterialTheme.typography.displayMedium, color = Fg, modifier = Modifier.weight(1f))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically, content = actions)
    }
}

/** Заголовок секции (.sec-h): 20 bold, справа — необязательная подпись. */
@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier, trailing: String = "") {
    Row(modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.Bottom) {
        Text(text, style = MaterialTheme.typography.titleLarge, color = Fg, modifier = Modifier.weight(1f))
        if (trailing.isNotEmpty()) Text(trailing, style = MaterialTheme.typography.bodySmall, color = Sec)
    }
}

@Composable
fun Dot(color: Color, size: Dp = 10.dp) {
    Box(Modifier.size(size).clip(CircleShape).background(color))
}

@Composable
fun Muted(text: String, modifier: Modifier = Modifier, align: TextAlign? = null) {
    Text(text, modifier = modifier, style = MaterialTheme.typography.bodySmall, color = Sec, textAlign = align)
}

// ------------------------------------------------------------ сгруппированные списки

/**
 * Группа настроек (.grouphead + .list): подпись сверху капсом, строки на карточке,
 * пояснение снизу. Разделители между строками — [GroupDivider].
 */
@Composable
fun Group(head: String = "", foot: String = "", headBadge: @Composable (() -> Unit)? = null, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (head.isNotEmpty()) {
            Row(Modifier.padding(horizontal = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(head.uppercase(), style = MaterialTheme.typography.bodySmall, color = Sec, letterSpacing = 0.3.sp)
                if (headBadge != null) {
                    Spacer(Modifier.width(6.dp))
                    headBadge()
                }
            }
        }
        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(RadiusCard)).background(Card), content = content)
        if (foot.isNotEmpty()) {
            Text(foot, style = MaterialTheme.typography.bodySmall, color = Sec, modifier = Modifier.padding(horizontal = 14.dp))
        }
    }
}

@Composable
fun GroupDivider() {
    Box(Modifier.fillMaxWidth().padding(start = 14.dp).height(1.dp).background(Hairline))
}

/** Цветной квадрат-значок строки (.li .ico). */
@Composable
fun RowIcon(icon: ImageVector, background: Color, tint: Color = Color.White) {
    Box(Modifier.size(30.dp).clip(RoundedCornerShape(8.dp)).background(background), contentAlignment = Alignment.Center) {
        Icon(icon, contentDescription = null, tint = tint, modifier = Modifier.size(17.dp))
    }
}

/** Строка группы: значок, заголовок с подписью, справа — значение или элемент. */
@Composable
fun ListRow(
    title: String,
    subtitle: String = "",
    icon: ImageVector? = null,
    iconBackground: Color = Tint,
    value: String = "",
    titleBadge: @Composable (() -> Unit)? = null,
    onClick: (() -> Unit)? = null,
    trailing: @Composable (RowScope.() -> Unit)? = null,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (icon != null) RowIcon(icon, iconBackground)
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(title, style = MaterialTheme.typography.bodyLarge, color = Fg, maxLines = 2, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false))
                if (titleBadge != null) {
                    Spacer(Modifier.width(6.dp))
                    titleBadge()
                }
            }
            if (subtitle.isNotEmpty()) Text(subtitle, style = MaterialTheme.typography.bodySmall, color = Sec)
        }
        if (value.isNotEmpty()) Text(value, style = MaterialTheme.typography.bodyMedium, color = Sec, maxLines = 1)
        if (trailing != null) trailing()
    }
}

/** Строка с переключателем (.tg): зелёный «включено», как в макете. */
@Composable
fun SwitchRow(
    title: String,
    hint: String = "",
    checked: Boolean,
    enabled: Boolean = true,
    titleBadge: @Composable (() -> Unit)? = null,
    onChange: (Boolean) -> Unit,
) {
    ListRow(title, hint, titleBadge = titleBadge) {
        Switch(
            checked = checked, onCheckedChange = onChange, enabled = enabled,
            colors = SwitchDefaults.colors(
                checkedTrackColor = Color(0xFF34C759), checkedThumbColor = Color.White, checkedBorderColor = Color.Transparent,
                uncheckedTrackColor = FillSoft, uncheckedThumbColor = Color.White, uncheckedBorderColor = Color.Transparent,
            ),
        )
    }
}

// ----------------------------------------------------------------- статусы и Pro

enum class Tone { GREEN, AMBER, RED, ACCENT, NEW, NEUTRAL }

enum class StatusIcon { CHECK, CLOCK, X, UNDO, MINUS, HAND, PLUS, BELL, CALENDAR, BLOCK }

/** Статус: всегда значок и слово, цвет только подкрепляет (не единственный носитель). */
data class StatusView(val label: String, val tone: Tone, val icon: StatusIcon)

fun statusIconVector(icon: StatusIcon): ImageVector = when (icon) {
    StatusIcon.CHECK -> Icons.Outlined.Check
    StatusIcon.CLOCK -> Icons.Outlined.AccessTime
    StatusIcon.X -> Icons.Outlined.Close
    StatusIcon.UNDO -> Icons.AutoMirrored.Outlined.Undo
    StatusIcon.MINUS -> Icons.Outlined.Remove
    StatusIcon.HAND -> Icons.Outlined.PanTool
    StatusIcon.PLUS -> Icons.Outlined.Add
    StatusIcon.BELL -> Icons.Outlined.NotificationsNone
    StatusIcon.CALENDAR -> Icons.Outlined.CalendarMonth
    StatusIcon.BLOCK -> Icons.Outlined.Block
}

/** Цвета тона: (фон, текст). */
fun toneColors(tone: Tone): Pair<Color, Color> = when (tone) {
    Tone.GREEN -> GreenSoft to Green
    Tone.AMBER -> AmberSoft to Amber
    Tone.RED -> RedSoft to Red
    Tone.ACCENT -> TintSoft to Tint
    Tone.NEW -> BrandAmber.copy(alpha = 0.22f) to Amber
    Tone.NEUTRAL -> FillSoft to Sec
}

@Composable
fun StatusPill(status: StatusView, modifier: Modifier = Modifier) {
    val (bg, fg) = toneColors(status.tone)
    Row(
        modifier.clip(RoundedCornerShape(50)).background(bg).padding(horizontal = 9.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(statusIconVector(status.icon), contentDescription = null, tint = fg, modifier = Modifier.size(13.dp))
        Spacer(Modifier.width(4.dp))
        Text(status.label, style = MaterialTheme.typography.labelSmall.copy(fontSize = 12.sp), color = fg, maxLines = 1)
    }
}

/** Круглая отметка статуса в ленте дня (.mark): значок в кружке + подпись для доступности. */
@Composable
fun StatusMark(status: StatusView) {
    val (bg, fg) = when (status.tone) {
        Tone.GREEN -> Green to Color.White
        Tone.NEW -> Tint to Color.White
        else -> toneColors(status.tone)
    }
    Box(Modifier.size(24.dp).clip(CircleShape).background(bg), contentAlignment = Alignment.Center) {
        Icon(statusIconVector(status.icon), contentDescription = status.label, tint = fg, modifier = Modifier.size(14.dp))
    }
}

/** Значок «Pro» у функций тарифа Pro. */
@Composable
fun ProBadge() {
    Box(
        Modifier.clip(RoundedCornerShape(6.dp)).background(Fg).padding(horizontal = 6.dp, vertical = 1.dp),
    ) {
        Text("Pro", color = Bg, style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.Bold)
    }
}

/** Сегментированный переключатель (.segc): варианты (значение → подпись). */
@Composable
fun Segmented(options: List<Pair<String, String>>, value: String, modifier: Modifier = Modifier, onChange: (String) -> Unit) {
    val outer = RoundedCornerShape(19.dp)
    val inner = RoundedCornerShape(16.dp)
    Row(modifier.fillMaxWidth().clip(outer).background(FillSoft).padding(2.dp)) {
        options.forEach { (key, label) ->
            val active = key == value
            Box(
                Modifier
                    .weight(1f)
                    .clip(inner)
                    .background(if (active) Card else Color.Transparent)
                    .clickable { if (!active) onChange(key) }
                    .padding(vertical = 7.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(label, style = MaterialTheme.typography.labelMedium, color = Fg, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center)
            }
        }
    }
}

/** Плашка-сообщение (ошибка, предупреждение, «нужен Pro»). */
@Composable
fun NoticeBox(text: String, tone: Tone = Tone.RED, modifier: Modifier = Modifier, actions: @Composable (RowScope.() -> Unit)? = null) {
    val (bg, fg) = toneColors(tone)
    Column(
        modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(bg).padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(text, style = MaterialTheme.typography.bodySmall, color = fg)
        if (actions != null) Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically, content = actions)
    }
}

/**
 * Окно снизу с отступом от краёв и большими скруглениями (.sheet v2): ручка,
 * шапка (необязательный значок слева, заголовок, подзаголовок, крестик).
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun InsetSheet(
    onDismiss: () -> Unit,
    title: String,
    subtitle: String = "",
    leading: @Composable (() -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = state,
        modifier = Modifier.padding(horizontal = 8.dp).navigationBarsPadding().padding(bottom = 8.dp),
        shape = RoundedCornerShape(RadiusShell),
        containerColor = Bg,
        dragHandle = {
            Box(Modifier.padding(top = 8.dp, bottom = 4.dp).size(width = 38.dp, height = 5.dp).clip(RoundedCornerShape(3.dp)).background(Ter))
        },
    ) {
        Column(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                if (leading != null) leading()
                Column(Modifier.weight(1f)) {
                    Text(title, style = MaterialTheme.typography.titleLarge, color = Fg, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    if (subtitle.isNotEmpty()) Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = Sec)
                }
                Box(
                    Modifier.size(30.dp).clip(CircleShape).background(FillSoft).clickable(onClick = onDismiss),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Outlined.Close, contentDescription = "Закрыть", tint = Sec, modifier = Modifier.size(15.dp))
                }
            }
            content()
        }
    }
}
