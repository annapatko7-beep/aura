package app.selfvoy.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Chat
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.EventAvailable
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.Handshake
import androidx.compose.material.icons.outlined.Mail
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.Card
import app.selfvoy.ui.theme.Hairline
import app.selfvoy.ui.theme.Tint
import app.selfvoy.ui.theme.FillSoft
import app.selfvoy.ui.theme.TintSoft
import app.selfvoy.ui.theme.Accent
import app.selfvoy.ui.theme.AccentSoft
import app.selfvoy.ui.theme.SelfvoyAvatar
import app.selfvoy.ui.theme.SelfvoyChip
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.Danger
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Lilac
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.Success
import app.selfvoy.ui.theme.TextMuted
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import app.selfvoy.ui.theme.Warning
import kotlinx.coroutines.delay

private const val MAX_ROUNDS = 5

private data class Step(val mine: Boolean, val text: String, val detail: String, val done: Boolean = false, val fail: Boolean = false)

/**
 * Переговоры Selfvoy (макет Meeting arrangement). Один agent.negotiate — один раунд;
 * шаги строятся из ответа сервера. Бронь — только по кнопке пользователя.
 * После 5 раундов без согласия решает человек (A2A_PROTOCOL.md).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun NegotiationsScreen() {
    val negotiation by AppStore.negotiation.collectAsState()
    val history by AppStore.negotiationHistory.collectAsState()
    val contacts by AppStore.contacts.collectAsState()
    val busy by AppStore.busy.collectAsState()
    val auth by AppStore.auth.collectAsState()
    val myName = firstName((auth as? AppStore.AuthState.LoggedIn)?.displayName.orEmpty())
    var composing by remember { mutableStateOf(negotiation == null) }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        val n = negotiation
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Организация\nвстречи", style = MaterialTheme.typography.headlineMedium, color = TextPrimary,
                modifier = Modifier.weight(1f))
            if (n != null && !composing) {
                GlassButton("Новые", { composing = true }, icon = Icons.Outlined.Add, height = 40.dp)
            }
        }
        // Страница записи: постоянная ссылка для людей без Selfvoy.
        GlassButton("Страница записи — пусть выбирают время сами", { AppStore.selectTab(AppStore.Tab.BOOKING) },
            Modifier.fillMaxWidth(), icon = Icons.Outlined.EventAvailable, quiet = true, height = 42.dp)

        if (n != null && !composing) {
            val theirName = firstName(contacts.firstOrNull { it.email == n.target || it.name == n.target }?.name ?: n.target)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                Pill(
                    when { busy -> "Идут"; n.booking != null -> "Забронировано"; n.accepted -> "Ждёт решения"; else -> "Нужен ваш выбор" },
                    when { n.booking != null -> Success; !n.accepted -> Warning; else -> Lilac },
                )
                Pill("Раунд ${n.round} из $MAX_ROUNDS", TextPrimary)
            }

            val steps = buildSteps(n, myName, theirName)
            var revealed by remember(n.at) { mutableIntStateOf(0) }
            LaunchedEffect(n.at) {
                while (revealed < steps.size) { delay(800); revealed++ }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                AgentCard(myName, "Selfvoy ${genitive(myName)}", talking = revealed < steps.size && steps.getOrNull(revealed)?.mine == true,
                    them = false, modifier = Modifier.weight(1f))
                AgentCard(theirName, "Selfvoy ${genitive(theirName)}", talking = revealed < steps.size && steps.getOrNull(revealed)?.mine == false,
                    them = true, modifier = Modifier.weight(1f))
            }
            if (n.topic.isNotEmpty()) Muted("Тема: ${n.topic}", Modifier.fillMaxWidth(), align = TextAlign.Center)

            steps.take(revealed).forEachIndexed { index, step -> StepRow(index + 1, step) }

            if (revealed >= steps.size) {
                if (n.accepted && n.slot != null) {
                    ResultCard(n)
                } else {
                    GlassCard(Modifier.fillMaxWidth()) {
                        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                            Text(if (n.round >= MAX_ROUNDS) "Лимит раундов исчерпан" else "Встречные варианты собеседника",
                                style = MaterialTheme.typography.titleMedium, color = TextPrimary)
                            Text(
                                if (n.round >= MAX_ROUNDS) "Оба Selfvoy провели $MAX_ROUNDS раундов без согласия. Дальше решаете вы — напишите собеседнику."
                                else "Selfvoy не договаривается дальше без вас: следующий раунд с другим окном или чат.",
                                style = MaterialTheme.typography.bodySmall, color = TextSecondary,
                            )
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                n.counterSlots.forEach { SelfvoyChip("${dayPhrase(it.start)}, ${timeOf(it.start)}", enabled = false) {} }
                            }
                            if (n.round < MAX_ROUNDS) {
                                GradientButton("Следующий раунд: две недели",
                                    { AppStore.negotiate(n.target, n.topic, n.durationMinutes, windowHours("week")) },
                                    Modifier.fillMaxWidth(), busy = busy)
                            }
                            GlassButton("Написать собеседнику", { AppStore.openChatByContact(n.target) },
                                Modifier.fillMaxWidth(), icon = Icons.AutoMirrored.Outlined.Chat)
                        }
                    }
                }
            }

            if (history.size > 1) {
                Text("Раунды за сессию", style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                history.drop(1).forEach { h ->
                    Muted("${h.target} · раунд ${h.round} — " + if (h.accepted) "согласовано: ${h.place?.name}" else "без согласия")
                }
            }
        } else {
            NegotiationForm(contacts, busy, onCancel = if (n != null) ({ composing = false }) else null) { target, topic, duration, window ->
                AppStore.negotiate(target, topic, duration, windowHours(window))
                composing = false
            }
        }
        Spacer(Modifier.height(8.dp))
    }
}

private fun buildSteps(n: AppStore.Negotiation, me: String, them: String): List<Step> {
    val selfvoyMe = "Selfvoy ${genitive(me)}"
    val selfvoyThem = "Selfvoy ${genitive(them)}"
    val steps = mutableListOf(
        Step(true, "$selfvoyMe предложил встречу: ${n.durationMinutes} мин",
            "окно — ${windowPhrase(n.windowHours)}"),
    )
    if (n.accepted && n.slot != null) {
        steps += Step(false, "$selfvoyThem проверил календарь", "общее окно: ${dayPhrase(n.slot.start)} в ${timeOf(n.slot.start)}")
        steps += Step(false, "$selfvoyThem предложил ${n.place?.name ?: "место"}",
            listOfNotNull(n.place?.city, n.place?.tags?.let { placeTags(it) }?.ifEmpty { null },
                n.place?.rating?.ifEmpty { null }?.let { "★ $it" }).joinToString(" · "))
        steps += Step(true, "$selfvoyMe принял место и время", n.message, done = true)
    } else {
        steps += Step(false, "$selfvoyThem проверил календарь", "в этом окне общего времени нет")
        steps += Step(false, if (n.counterSlots.isEmpty()) "Встречных вариантов нет" else "Встречные варианты: ${n.counterSlots.size}",
            n.message.ifEmpty { "нужно ваше решение" }, fail = true)
    }
    return steps
}

@Composable
private fun Pill(text: String, color: Color) {
    Box(Modifier.clip(RoundedCornerShape(50)).border(1.dp, color.copy(alpha = 0.5f), RoundedCornerShape(50))
        .padding(horizontal = 12.dp, vertical = 5.dp)) {
        Text(text, style = MaterialTheme.typography.labelMedium, color = color)
    }
}

@Composable
private fun AgentCard(name: String, selfvoy: String, talking: Boolean, them: Boolean, modifier: Modifier) {
    val shape = RoundedCornerShape(22.dp)
    val bars = remember(them) { List(18) { 0.25f + kotlin.math.abs(kotlin.math.sin(it * 1.7 + if (them) 2.0 else 0.0)).toFloat() * 0.65f } }
    Column(
        modifier
            .clip(shape)
            .background(Card)
            .border(1.dp, if (them) Hairline else Tint.copy(alpha = 0.35f), shape)
            .padding(14.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        SelfvoyAvatar(name, size = 56.dp, ring = true)
        Spacer(Modifier.height(6.dp))
        Text(name, style = MaterialTheme.typography.titleMedium, color = TextPrimary)
        Text(selfvoy, style = MaterialTheme.typography.bodySmall, color = TextSecondary)
        Row(
            Modifier.padding(top = 8.dp).fillMaxWidth().height(52.dp).clip(RoundedCornerShape(12.dp))
                .background(FillSoft).padding(8.dp),
            horizontalArrangement = Arrangement.spacedBy(2.dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            bars.forEach { level ->
                Box(Modifier.weight(1f).fillMaxWidth().height((36 * if (talking) level else level * 0.35f).dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(Brush.verticalGradient(listOf(Accent, Color(0xFF6366F1)))))
            }
        }
    }
}

@Composable
private fun StepRow(number: Int, step: Step) {
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = if (step.mine) Arrangement.Start else Arrangement.End,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val badge = @Composable {
            Box(
                Modifier.size(32.dp).clip(CircleShape)
                    .background((if (step.fail) Danger else Accent).copy(alpha = 0.15f))
                    .border(1.5.dp, if (step.fail) Danger else AccentSoft, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                if (step.done) Icon(Icons.Outlined.Check, contentDescription = null, tint = TextPrimary, modifier = Modifier.size(16.dp))
                else Text("$number", style = MaterialTheme.typography.labelLarge, color = TextPrimary)
            }
        }
        if (step.mine) { badge(); Spacer(Modifier.width(10.dp)) }
        Column(
            Modifier.weight(1f, fill = false).clip(RoundedCornerShape(20.dp)).background(Card)
                .border(1.dp, Hairline, RoundedCornerShape(20.dp)).padding(horizontal = 14.dp, vertical = 9.dp),
            horizontalAlignment = if (step.mine) Alignment.Start else Alignment.End,
        ) {
            Text(step.text, style = MaterialTheme.typography.labelMedium, color = TextPrimary,
                textAlign = if (step.mine) TextAlign.Start else TextAlign.End)
            if (step.detail.isNotEmpty()) Text(step.detail, style = MaterialTheme.typography.labelSmall, color = TextMuted,
                textAlign = if (step.mine) TextAlign.Start else TextAlign.End)
        }
        if (!step.mine) { Spacer(Modifier.width(10.dp)); badge() }
    }
}

@Composable
private fun ResultCard(n: AppStore.Negotiation) {
    val busy by AppStore.busy.collectAsState()
    val slot = n.slot ?: return
    val shape = RoundedCornerShape(24.dp)
    Column(
        Modifier.fillMaxWidth().clip(shape)
            .background(TintSoft)
            .border(1.dp, (if (n.booking != null) Success else Accent).copy(alpha = 0.45f), shape)
            .padding(22.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text("${dayTitle(slot.start)}, ${timeOf(slot.start)}\n${n.place?.name.orEmpty()}",
            style = MaterialTheme.typography.headlineMedium, color = TextPrimary)
        Muted(listOfNotNull("Двое", "до ${timeOf(slot.end)}", n.place?.city?.ifEmpty { null },
            n.place?.rating?.ifEmpty { null }?.let { "рейтинг $it" }).joinToString("  ·  "))
        if (n.booking != null) {
            Text("Столик забронирован · код ${n.booking} · встреча в задачах",
                style = MaterialTheme.typography.labelMedium, color = Success)
            GlassButton("Открыть задачи", { AppStore.selectTab(AppStore.Tab.TASKS) }, Modifier.fillMaxWidth())
        } else {
            GradientButton("Подтвердить и забронировать", { AppStore.bookNegotiation() }, Modifier.fillMaxWidth(),
                icon = Icons.Outlined.Handshake, busy = busy)
            GlassButton("Остановить", { AppStore.clearNegotiation() }, Modifier.fillMaxWidth(), quiet = true)
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun NegotiationForm(
    contacts: List<AppStore.Contact>,
    busy: Boolean,
    onCancel: (() -> Unit)?,
    onStart: (target: String, topic: String, duration: Int, window: String) -> Unit,
) {
    var target by remember { mutableStateOf(contacts.firstOrNull()?.email.orEmpty()) }
    var email by remember { mutableStateOf("") }
    var topic by remember { mutableStateOf("") }
    var window by remember { mutableStateOf("weekend") }
    var duration by remember { mutableIntStateOf(60) }

    GlassCard(Modifier.fillMaxWidth(), radius = 26.dp, contentPadding = androidx.compose.foundation.layout.PaddingValues(22.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Text("С кем договориться?", style = MaterialTheme.typography.titleMedium, color = TextPrimary)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                contacts.forEach { c -> SelfvoyChip(c.name, active = target == c.email && email.isEmpty()) { target = c.email; email = "" } }
            }
            SelfvoyField(email, { email = it; if (it.isNotBlank()) target = it }, "или email собеседника", trailing = Icons.Outlined.Mail)
            SelfvoyField(topic, { topic = it }, "Тема, например «обсудить стартап»")
            Text("Когда", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf("tomorrow" to "сегодня–завтра", "weekend" to "до конца недели", "week" to "две недели")
                    .forEach { (k, l) -> SelfvoyChip(l, active = window == k) { window = k } }
            }
            Text("Длительность", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(30, 60, 90).forEach { d -> SelfvoyChip("$d мин", active = duration == d) { duration = d } }
            }
            GradientButton("Начать переговоры", { onStart(target, topic, duration, window) }, Modifier.fillMaxWidth(),
                icon = Icons.Outlined.Handshake, enabled = target.isNotBlank() && !busy, busy = busy)
            if (onCancel != null) GlassButton("Отмена", onCancel, Modifier.fillMaxWidth(), quiet = true)
            Muted("Оба Selfvoy сверят свободное время и подберут место с учётом питания обоих. Бронь — только после вашего подтверждения.")
        }
    }
}
