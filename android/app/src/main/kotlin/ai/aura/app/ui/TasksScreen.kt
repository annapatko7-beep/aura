package ai.aura.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.GlassPanel
import ai.aura.app.ui.theme.TextMuted

/** Задачи и напоминания: список, создание (с remind_at), статусы, удаление. */
@Composable
fun TasksScreen() {
    val tasks by AppStore.tasks.collectAsState()
    var title by remember { mutableStateOf("") }
    var remindAt by remember { mutableStateOf("") }
    var creating by remember { mutableStateOf(false) }

    Scaffold(
        floatingActionButton = {
            FloatingActionButton(onClick = { creating = !creating }) {
                Icon(Icons.Filled.Add, contentDescription = "Новая задача")
            }
        },
    ) { padding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (creating) {
                item {
                    GlassPanel(modifier = Modifier.fillMaxWidth()) {
                        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedTextField(
                                value = title, onValueChange = { title = it },
                                label = { Text("Что нужно сделать?") },
                                modifier = Modifier.fillMaxWidth(),
                            )
                            OutlinedTextField(
                                value = remindAt, onValueChange = { remindAt = it },
                                label = { Text("Напомнить (2026-09-21T09:00:00Z, можно пусто)") },
                                modifier = Modifier.fillMaxWidth(),
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                TextButton(onClick = {
                                    AppStore.createTask(title, remindAt)
                                    title = ""; remindAt = ""; creating = false
                                }) { Text("Создать") }
                                TextButton(onClick = { creating = false }) { Text("Отмена") }
                            }
                        }
                    }
                }
            }
            items(tasks, key = { it.id }) { task ->
                GlassPanel(modifier = Modifier.fillMaxWidth()) {
                    Column {
                        Text(task.title, style = MaterialTheme.typography.titleSmall)
                        if (task.notes.isNotEmpty()) {
                            Text(task.notes, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        Text(
                            buildString {
                                append(task.status)
                                if (task.remindAt.isNotEmpty()) append(" · напомнить ${task.remindAt}")
                            },
                            color = TextMuted,
                            style = MaterialTheme.typography.labelSmall,
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            if (task.status == "pending") {
                                TextButton(onClick = { AppStore.setTaskStatus(task.id, "complete") }) {
                                    Text("Готово")
                                }
                                TextButton(onClick = { AppStore.setTaskStatus(task.id, "cancel") }) {
                                    Text("Отменить")
                                }
                            } else {
                                TextButton(onClick = { AppStore.setTaskStatus(task.id, "reopen") }) {
                                    Text("Вернуть")
                                }
                            }
                            TextButton(onClick = { AppStore.deleteTask(task.id) }) { Text("Удалить") }
                        }
                    }
                }
            }
        }
    }
}
