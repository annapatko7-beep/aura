package ai.aura.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
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
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.AccentSoft
import ai.aura.app.ui.theme.GlassPanel

/** Чаты Ауры: список диалогов + лента сообщений + поле ввода + голос. */
@Composable
fun ChatsScreen() {
    val chats by AppStore.chats.collectAsState()
    val openChatId by AppStore.openChatId.collectAsState()
    val messages by AppStore.messages.collectAsState()
    val askReply by AppStore.askReply.collectAsState()
    var draft by remember { mutableStateOf("") }
    var askText by remember { mutableStateOf("") }
    val listState = rememberLazyListState()

    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) listState.animateScrollToItem(messages.size - 1)
    }

    Scaffold(
        floatingActionButton = {
            FloatingActionButton(onClick = { AppStore.requestVoice() }) {
                Icon(Icons.Filled.Mic, contentDescription = "Спросить голосом")
            }
        },
    ) { padding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp),
            state = listState,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            item {
                GlassPanel(modifier = Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("Спросить Ауру", style = MaterialTheme.typography.titleMedium)
                        OutlinedTextField(
                            value = askText, onValueChange = { askText = it },
                            label = { Text("Например: напомни полить цветы вечером") },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = {
                                AppStore.ask(askText)
                                askText = ""
                            }) { Text("Спросить") }
                            TextButton(onClick = { AppStore.requestVoice() }) { Text("Голосом") }
                        }
                        if (askReply.isNotEmpty()) {
                            Text(askReply, color = AccentSoft)
                        }
                    }
                }
            }

            if (openChatId == 0L) {
                item {
                    Text("Диалоги", style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.padding(top = 8.dp))
                }
                items(chats, key = { it.id }) { chat ->
                    GlassPanel(modifier = Modifier.fillMaxWidth()) {
                        Column {
                            Text(chat.title, style = MaterialTheme.typography.titleSmall)
                            if (chat.lastBody.isNotEmpty()) {
                                Text(chat.lastBody, maxLines = 1, overflow = TextOverflow.Ellipsis,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            TextButton(onClick = { AppStore.openChat(chat.id) }) { Text("Открыть") }
                        }
                    }
                }
            } else {
                items(messages, key = { it.id }) { message ->
                    GlassPanel(modifier = Modifier.fillMaxWidth()) {
                        Column {
                            Text(
                                if (message.kind == "agent_reply") "Аура" else "Сообщение",
                                style = MaterialTheme.typography.labelSmall,
                                color = if (message.kind == "agent_reply") AccentSoft
                                else MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(message.body)
                        }
                    }
                }
                item {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        OutlinedTextField(
                            value = draft, onValueChange = { draft = it },
                            label = { Text("Сообщение") },
                            modifier = Modifier.weight(1f),
                        )
                        IconButton(onClick = {
                            AppStore.sendMessage(draft)
                            draft = ""
                        }) { Icon(Icons.AutoMirrored.Filled.Chat, contentDescription = "Отправить") }
                    }
                }
            }
        }
    }
}
