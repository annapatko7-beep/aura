package ai.aura.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.GlassPanel
import ai.aura.app.ui.theme.TextMuted
import ai.aura.app.ui.theme.statusColor

/** «Входящая» уведомлений (этап 13) + push-устройства с отзывом. */
@Composable
fun NotificationsScreen() {
    val notices by AppStore.notices.collectAsState()
    val devices by AppStore.pushDevices.collectAsState()
    val pushStatus by AppStore.pushStatus.collectAsState()

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text("Входящая", style = MaterialTheme.typography.titleMedium,
                    modifier = Modifier.padding(top = 8.dp))
                TextButton(onClick = { AppStore.markAllRead() }) { Text("Прочитать все") }
            }
        }
        items(notices, key = { it.id }) { notice ->
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column {
                    Text(notice.title, style = MaterialTheme.typography.titleSmall,
                        color = statusColor(notice.kind))
                    Text(notice.body, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(horizontalArrangement = Arrangement.SpaceBetween,
                        modifier = Modifier.fillMaxWidth()) {
                        Text(
                            buildString {
                                append(notice.kind)
                                if (!notice.read) append(" · не прочитано")
                                append(" · ${notice.createdAt}")
                            },
                            color = TextMuted,
                            style = MaterialTheme.typography.labelSmall,
                        )
                        if (!notice.read) {
                            TextButton(onClick = { AppStore.markRead(notice.id) }) {
                                Text("Прочитано")
                            }
                        }
                    }
                }
            }
        }
        item {
            Text("Push-устройства", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(top = 16.dp))
            Text(pushStatus.ifEmpty { "Push ещё не регистрировался" },
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        items(devices, key = { it.id }) { device ->
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text(
                        "${device.platform} · ${if (device.enabled) "включено" else "выключено"}",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    TextButton(onClick = { AppStore.revokePushDevice(device.id) }) {
                        Text("Отозвать")
                    }
                }
            }
        }
    }
}
