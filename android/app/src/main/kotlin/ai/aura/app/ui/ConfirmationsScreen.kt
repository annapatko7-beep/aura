package ai.aura.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.Danger
import ai.aura.app.ui.theme.GlassPanel
import ai.aura.app.ui.theme.Warning

/**
 * «Ждут решения»: отложенные действия Ауры (барьер подтверждения, этап 8) и
 * разрешения инструментов. Ничего не исполняется без явного «Разрешить».
 */
@Composable
fun ConfirmationsScreen() {
    val confirmations by AppStore.confirmations.collectAsState()
    val permissions by AppStore.permissions.collectAsState()

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            Text("Ждут подтверждения", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(top = 8.dp))
        }
        if (confirmations.isEmpty()) {
            item {
                Text("Опасных операций в очереди нет",
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        items(confirmations, key = { it.id }) { action ->
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(action.summary.ifEmpty { action.tool },
                        style = MaterialTheme.typography.titleSmall, color = Warning)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = { AppStore.decide(action.id, approve = true) }) {
                            Text("Разрешить")
                        }
                        OutlinedButton(onClick = { AppStore.decide(action.id, approve = false) }) {
                            Text("Отклонить")
                        }
                    }
                }
            }
        }
        item {
            Text("Разрешения инструментов", style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(top = 16.dp))
        }
        items(permissions, key = { it.tool }) { row ->
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column {
                    Text(row.tool + if (row.dangerous) " (опасный)" else "",
                        style = MaterialTheme.typography.titleSmall,
                        color = if (row.dangerous) Danger else MaterialTheme.colorScheme.onSurface)
                    Text(row.description, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        listOf("allow" to "Всегда", "ask" to "Спрашивать", "deny" to "Запретить")
                            .forEach { (mode, label) ->
                                if (row.mode == mode) {
                                    Button(onClick = {}) { Text(label) }
                                } else {
                                    OutlinedButton(onClick = { AppStore.setPermission(row.tool, mode) }) {
                                        Text(label)
                                    }
                                }
                            }
                    }
                }
            }
        }
    }
}
