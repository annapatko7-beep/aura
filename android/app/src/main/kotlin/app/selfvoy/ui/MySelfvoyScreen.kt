package app.selfvoy.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Send
import androidx.compose.material.icons.outlined.Mic
import androidx.compose.material.icons.outlined.Psychology
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.TintSoft
import app.selfvoy.ui.theme.Card
import app.selfvoy.ui.theme.SelfvoyChip
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.SelfvoyOrb
import app.selfvoy.ui.theme.BgElevated
import app.selfvoy.ui.theme.Danger
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.RadiusBubble
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import app.selfvoy.ui.theme.Warning

/**
 * «Мой Selfvoy» (макет Chat): реплики пользователя слева, ответы Selfvoy справа.
 * Опасные действия (письмо, сообщение, бронь) Selfvoy не выполняет сам — под
 * ответом кнопки «Подтвердить / Отклонить» (confirmation.approve/deny).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun MySelfvoyScreen() {
    val dialog by AppStore.dialog.collectAsState()
    val busy by AppStore.busy.collectAsState()
    val confirmations by AppStore.confirmations.collectAsState()
    val auth by AppStore.auth.collectAsState()
    val pendingIds = confirmations.map { it.id }.toSet()
    var draft by remember { mutableStateOf("") }
    var showMemory by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    val name = firstName((auth as? AppStore.AuthState.LoggedIn)?.displayName.orEmpty())

    LaunchedEffect(dialog.size) { if (dialog.isNotEmpty()) listState.animateScrollToItem(dialog.size) }

    fun send(text: String) {
        if (text.isBlank() || busy) return
        AppStore.ask(text)
        draft = ""
    }

    Column(Modifier.fillMaxSize().imePadding()) {
        // Шапка
        Row(
            Modifier
                .fillMaxWidth()
                .background(Brush.verticalGradient(listOf(TintSoft, Color.Transparent)))
                .padding(horizontal = 20.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SelfvoyOrb(52.dp)
            Column(Modifier.weight(1f).padding(start = 14.dp)) {
                Text("Selfvoy", style = MaterialTheme.typography.headlineSmall, color = TextPrimary)
                Text((if (busy) "Думает" else "В сети") + " · Персональный ассистент",
                    style = MaterialTheme.typography.bodySmall, color = TextSecondary)
            }
            RoundIconButton(Icons.Outlined.Psychology, "Память", {
                AppStore.refreshMemory(); showMemory = true
            }, size = 42.dp)
        }

        LazyColumn(
            Modifier.weight(1f).fillMaxWidth(),
            state = listState,
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 16.dp, vertical = 18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            item {
                if (dialog.isEmpty()) {
                    Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(10.dp),
                        modifier = Modifier.fillMaxWidth()) {
                        Bubble(fromSelfvoy = true, text = "Привет, $name! Я ваш Selfvoy. Скажите, что нужно сделать, — " +
                            "и я договорюсь с Selfvoy других людей, поставлю напоминание или подготовлю письмо.")
                        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                            verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            listOf("Хочу встретиться с Димой в эти выходные и обсудить стартап",
                                "Что у меня сегодня в расписании?", "Напомни завтра в 10 позвонить юристу",
                            ).forEach { SelfvoyChip(it) { send(it) } }
                        }
                    }
                }
            }
            items(dialog, key = { it.id }) { entry ->
                Box(Modifier.fillMaxWidth(), contentAlignment = if (entry.fromSelfvoy) Alignment.CenterEnd else Alignment.CenterStart) {
                    Bubble(fromSelfvoy = entry.fromSelfvoy, text = entry.text, error = entry.error) {
                        entry.actions.forEach { action ->
                            val open = action.confirmationId in pendingIds
                            Text((if (open) "Ждёт вашего решения: " else "Решено: ") + action.summary,
                                style = MaterialTheme.typography.bodySmall, color = if (open) Warning else TextSecondary)
                            if (open) {
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    SelfvoyChip("Подтвердить") { AppStore.decide(action.confirmationId, true) }
                                    SelfvoyChip("Отклонить") { AppStore.decide(action.confirmationId, false) }
                                }
                            }
                        }
                        if (entry.hasA2a) SelfvoyChip("Открыть переговоры") { AppStore.selectTab(AppStore.Tab.NEGOTIATIONS) }
                    }
                }
            }
            if (busy) item { Muted("Selfvoy думает…", Modifier.fillMaxWidth().padding(end = 4.dp), align = androidx.compose.ui.text.style.TextAlign.End) }
        }

        // Ввод
        Row(
            Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, bottom = 12.dp, top = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            RoundIconButton(Icons.Outlined.Mic, "Сказать голосом", { AppStore.requestVoice() }, size = 48.dp)
            SelfvoyField(draft, { draft = it }, "Напишите что-нибудь…", Modifier.weight(1f),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send), onDone = { send(draft) })
            RoundIconButton(Icons.AutoMirrored.Outlined.Send, "Отправить", { send(draft) }, accent = true, size = 50.dp,
                enabled = draft.isNotBlank() && !busy)
        }
    }

    if (showMemory) MemoryDialog { showMemory = false }
}

/** Реплика: пользователь — тёмное стекло, Selfvoy — светлое стекло (макет Chat). */
@Composable
fun Bubble(
    fromSelfvoy: Boolean,
    text: String,
    error: Boolean = false,
    extra: (@Composable () -> Unit)? = null,
) {
    val shape = RoundedCornerShape(RadiusBubble)
    Column(
        Modifier
            .widthIn(max = 320.dp)
            .clip(shape)
            // «Жидкое стекло»: тонировка (у Selfvoy — сиреневая), блик сверху и
            // светлый кант по верхнему краю.
            // Пузырь ассистента — карточка, свой — мягкий акцент (.bub.ai / .bub.me).
            .background(if (fromSelfvoy) Card else TintSoft)
            .border(1.dp, if (error) Danger.copy(alpha = 0.6f) else Color.Transparent, shape)
            .padding(horizontal = 18.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Text(text, style = MaterialTheme.typography.bodyMedium, color = if (error) Danger else TextPrimary)
        extra?.invoke()
    }
}

@Composable
private fun MemoryDialog(onDismiss: () -> Unit) {
    val memory by AppStore.memory.collectAsState()
    val prefs by AppStore.prefs.collectAsState()
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = BgElevated,
        confirmButton = { TextButton(onClick = onDismiss) { Text("Закрыть") } },
        title = { Text("Что Selfvoy знает о вас", style = MaterialTheme.typography.titleMedium) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                val city = prefs?.get("city")?.toString()?.trim('"').orEmpty()
                Muted("Город"); Text(city.ifEmpty { "—" }, color = TextPrimary)
                Muted("Факты из разговоров")
                if (memory.isEmpty()) Text("Пока ничего", color = TextSecondary)
                memory.take(8).forEach { Text("• ${it.text}", color = TextSecondary, style = MaterialTheme.typography.bodySmall) }
                Muted("Другие Selfvoy видят только, свободны ли вы в предложенное время, — не ваш календарь и не эти записи.")
            }
        },
    )
}
