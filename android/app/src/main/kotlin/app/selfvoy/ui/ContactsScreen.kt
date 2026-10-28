package app.selfvoy.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.automirrored.outlined.Chat
import androidx.compose.material.icons.automirrored.outlined.Send
import androidx.compose.material.icons.outlined.AutoAwesome
import androidx.compose.material.icons.outlined.CalendarMonth
import androidx.compose.material.icons.outlined.Handshake
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.PersonAdd
import androidx.compose.material.icons.outlined.Search
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
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
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.Hairline
import app.selfvoy.ui.theme.Card
import app.selfvoy.ui.theme.Accent
import app.selfvoy.ui.theme.AccentSoft
import app.selfvoy.ui.theme.SelfvoyAvatar
import app.selfvoy.ui.theme.SelfvoyChip
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.Dot
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.Lilac
import app.selfvoy.ui.theme.Manrope
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.TextMuted
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import kotlinx.coroutines.delay

/**
 * Контакты (макет Contacts): люди из чатов, поиск новых (users.search → chat.open),
 * карточка человека с «Начать переговоры» / «Открыть чат» и сама переписка.
 */
@Composable
fun ContactsScreen() {
    val contacts by AppStore.contacts.collectAsState()
    val found by AppStore.userResults.collectAsState()
    val openChatId by AppStore.openChatId.collectAsState()
    var query by remember { mutableStateOf("") }
    var recentOnly by remember { mutableStateOf(false) }
    var selected by remember { mutableStateOf<AppStore.Contact?>(null) }
    val searchFocus = remember { FocusRequester() }

    LaunchedEffect(query) { delay(300); AppStore.searchUsers(query) }

    if (openChatId != 0L) {
        ChatView(openChatId, contacts.firstOrNull { it.chatId == openChatId }?.name.orEmpty())
        return
    }
    selected?.let { contact ->
        BackHandler { selected = null }
        ContactCard(contact) { selected = null }
        return
    }

    val visible = contacts.filter { c ->
        (!recentOnly || c.lastAt.isNotEmpty()) &&
            (query.isBlank() || c.name.contains(query, true) || c.email.contains(query, true))
    }
    val newPeople = found.filter { f -> contacts.none { it.userId == f.id } }

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 20.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        item { ScreenHeader("Контакты") }
        item {
            Column(Modifier.padding(top = 10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Outlined.Search, contentDescription = null, tint = TextSecondary, modifier = Modifier.size(20.dp))
                    Spacer(Modifier.width(10.dp))
                    BasicTextField(
                        value = query, onValueChange = { query = it }, singleLine = true,
                        textStyle = TextStyle(fontFamily = Manrope, fontSize = 17.sp, color = TextPrimary),
                        cursorBrush = SolidColor(Lilac), modifier = Modifier.weight(1f).focusRequester(searchFocus),
                        decorationBox = { inner ->
                            Box { if (query.isEmpty()) Text("Искать людей или Selfvoy", color = TextMuted, fontFamily = Manrope, fontSize = 17.sp); inner() }
                        },
                    )
                }
                HorizontalDivider(Modifier.padding(top = 12.dp), color = Hairline)
            }
        }
        item {
            Row(Modifier.padding(vertical = 12.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                SelfvoyChip("Все", active = !recentOnly) { recentOnly = false }
                SelfvoyChip("Недавние", active = recentOnly) { recentOnly = true }
            }
        }
        if (visible.isEmpty() && newPeople.isEmpty()) {
            item { Muted(if (query.isNotBlank()) "Никого не нашли" else "Пока нет собеседников. Найдите человека по имени или email.") }
        }
        items(visible, key = { it.userId }) { contact ->
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).clickable { selected = contact }.padding(vertical = 10.dp, horizontal = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                SelfvoyAvatar(contact.name, size = 46.dp)
                Column(Modifier.weight(1f).padding(horizontal = 14.dp)) {
                    Text(contact.name, style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                    Text(contact.lastBody.ifEmpty { "Selfvoy доступен" }, style = MaterialTheme.typography.bodySmall,
                        color = TextSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                Dot(AccentSoft)
            }
        }
        if (newPeople.isNotEmpty()) {
            item { Text("Найдено в Selfvoy", style = MaterialTheme.typography.labelMedium, color = TextSecondary, modifier = Modifier.padding(top = 12.dp)) }
            items(newPeople, key = { "found-${it.id}" }) { user ->
                Row(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    SelfvoyAvatar(user.name, size = 38.dp)
                    Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
                        Text(user.name, style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                        Muted(if (user.online) "в сети" else "есть Selfvoy")
                    }
                    // chat.open принимает id пользователя строкой.
                    GlassButton("Добавить", { AppStore.openChatByContact(user.id.toString()); query = "" }, icon = Icons.Outlined.PersonAdd, height = 36.dp)
                }
            }
        }
        item {
            GlassButton("Пригласить в Selfvoy", { searchFocus.requestFocus() }, Modifier.padding(top = 20.dp), icon = Icons.Outlined.PersonAdd, height = 48.dp)
            Muted("Найдите человека по email — если у него есть Selfvoy, он появится в списке.", Modifier.padding(top = 8.dp))
        }
    }
}

@Composable
private fun ContactCard(contact: AppStore.Contact, onBack: () -> Unit) {
    val permissions by AppStore.permissions.collectAsState()
    val busy by AppStore.busy.collectAsState()
    fun mode(tool: String) = when (permissions.firstOrNull { it.tool == tool }?.mode) {
        "allow" -> "сам"; "ask" -> "с вашего подтверждения"; "deny" -> "запрещено"; else -> "—"
    }
    Column(Modifier.fillMaxSize().padding(20.dp), verticalArrangement = Arrangement.spacedBy(18.dp)) {
        RoundIconButton(Icons.AutoMirrored.Outlined.ArrowBack, "Назад", onBack, size = 40.dp)
        val shape = RoundedCornerShape(30.dp)
        Column(
            Modifier.fillMaxWidth().clip(shape)
                .background(Card)
                .border(1.dp, Hairline, shape).padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                SelfvoyAvatar(contact.name, size = 96.dp, ring = true)
                Spacer(Modifier.height(12.dp))
                Text(contact.name, style = MaterialTheme.typography.headlineSmall, color = TextPrimary)
                Text("Selfvoy доступен · ${contact.email}", style = MaterialTheme.typography.bodySmall, color = TextSecondary)
            }
            InfoBlock(Icons.Outlined.CalendarMonth, "Последнее в переписке",
                if (contact.lastBody.isNotEmpty()) listOf("«${contact.lastBody}»", "${dayPhrase(contact.lastAt)}, ${timeOf(contact.lastAt)}")
                else listOf("Пока ничего"))
            InfoBlock(Icons.Outlined.Lock, "Что может ваш Selfvoy",
                listOf("Писать: ${mode("send_message")}", "Бронировать: ${mode("book_table")}", "Делиться: только свободными окнами"))
            GlassButton("Начать переговоры", {
                AppStore.negotiate(contact.email, "", 60, windowHours("weekend"))
            }, Modifier.fillMaxWidth(), icon = Icons.Outlined.Handshake, enabled = !busy, height = 50.dp)
            GlassButton("Открыть чат", { AppStore.openChat(contact.chatId) }, Modifier.fillMaxWidth(),
                icon = Icons.AutoMirrored.Outlined.Chat, quiet = true)
        }
    }
}

@Composable
private fun InfoBlock(icon: androidx.compose.ui.graphics.vector.ImageVector, title: String, lines: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(28.dp).clip(RoundedCornerShape(14.dp)).background(Accent), contentAlignment = Alignment.Center) {
                Icon(icon, contentDescription = null, tint = Color.White, modifier = Modifier.size(15.dp))
            }
            Text(title, style = MaterialTheme.typography.titleSmall, color = TextPrimary, modifier = Modifier.padding(start = 10.dp))
        }
        lines.forEach { Text(it, style = MaterialTheme.typography.labelMedium, color = TextPrimary, modifier = Modifier.padding(start = 38.dp)) }
    }
}

/** Переписка с человеком; режим «Спросить Selfvoy» отправляет agent.ask в этот чат. */
@Composable
private fun ChatView(chatId: Long, title: String) {
    val messages by AppStore.messages.collectAsState()
    val userId by AppStore.userId.collectAsState()
    val busy by AppStore.busy.collectAsState()
    var draft by remember { mutableStateOf("") }
    var toSelfvoy by remember { mutableStateOf(false) }
    val listState = rememberLazyListState()
    BackHandler { AppStore.closeChat() }
    LaunchedEffect(messages.size) { if (messages.isNotEmpty()) listState.animateScrollToItem(messages.size - 1) }

    fun send() {
        if (draft.isBlank() || busy) return
        if (toSelfvoy) AppStore.ask(draft, chatId) else AppStore.sendMessage(draft)
        draft = ""
    }

    Column(Modifier.fillMaxSize().imePadding()) {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            RoundIconButton(Icons.AutoMirrored.Outlined.ArrowBack, "Назад", { AppStore.closeChat() }, size = 38.dp)
            Spacer(Modifier.width(12.dp))
            SelfvoyAvatar(title, size = 40.dp)
            Text(title, style = MaterialTheme.typography.titleMedium, color = TextPrimary, modifier = Modifier.padding(start = 12.dp))
        }
        HorizontalDivider(color = Hairline)
        LazyColumn(
            Modifier.weight(1f).fillMaxWidth(), state = listState,
            contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            if (messages.isEmpty()) item { Muted("Сообщений пока нет. Напишите или попросите Selfvoy договориться о встрече.") }
            items(messages, key = { it.id }) { message ->
                val mine = message.senderId == userId
                val agent = message.kind.startsWith("agent")
                Column(Modifier.fillMaxWidth(), horizontalAlignment = if (mine) Alignment.End else Alignment.Start) {
                    if (agent) Text("Selfvoy", style = MaterialTheme.typography.labelSmall, color = AccentSoft)
                    Bubble(fromSelfvoy = mine || agent, text = message.body)
                    Muted("${message.senderName.ifEmpty { if (mine) "Вы" else title }} · ${timeOf(message.createdAt)}")
                }
            }
        }
        Row(Modifier.padding(horizontal = 16.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            SelfvoyChip("Обычное", active = !toSelfvoy) { toSelfvoy = false }
            SelfvoyChip("Спросить Selfvoy", active = toSelfvoy) { toSelfvoy = true }
        }
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            SelfvoyField(draft, { draft = it }, if (toSelfvoy) "Например: договорись о встрече" else "Сообщение…", Modifier.weight(1f),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send), onDone = { send() })
            RoundIconButton(if (toSelfvoy) Icons.Outlined.AutoAwesome else Icons.AutoMirrored.Outlined.Send, "Отправить",
                { send() }, accent = true, size = 48.dp, enabled = draft.isNotBlank() && !busy)
        }
    }
}
