package ai.aura.app.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Chat
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Notifications
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Task
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import ai.aura.app.AppStore

/** Нижняя навигация: Чаты / Задачи / Ждут / Входящая / Настройки. */
@Composable
fun MainScaffold() {
    val tab by AppStore.tab.collectAsState()
    val unread by AppStore.unread.collectAsState()
    val confirmations by AppStore.confirmations.collectAsState()

    Scaffold(
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == AppStore.Tab.CHATS,
                    onClick = { AppStore.selectTab(AppStore.Tab.CHATS) },
                    icon = { Icon(Icons.AutoMirrored.Filled.Chat, contentDescription = "Чаты") },
                    label = { Text("Чаты") },
                )
                NavigationBarItem(
                    selected = tab == AppStore.Tab.TASKS,
                    onClick = { AppStore.selectTab(AppStore.Tab.TASKS) },
                    icon = { Icon(Icons.Filled.Task, contentDescription = "Задачи") },
                    label = { Text("Задачи") },
                )
                NavigationBarItem(
                    selected = tab == AppStore.Tab.PENDING,
                    onClick = { AppStore.selectTab(AppStore.Tab.PENDING) },
                    icon = {
                        BadgedBox(badge = {
                            if (confirmations.isNotEmpty()) Badge { Text("${confirmations.size}") }
                        }) { Icon(Icons.Filled.CheckCircle, contentDescription = "Ждут решения") }
                    },
                    label = { Text("Ждут") },
                )
                NavigationBarItem(
                    selected = tab == AppStore.Tab.NOTICES,
                    onClick = { AppStore.selectTab(AppStore.Tab.NOTICES) },
                    icon = {
                        BadgedBox(badge = {
                            if (unread > 0) Badge { Text("$unread") }
                        }) { Icon(Icons.Filled.Notifications, contentDescription = "Входящая") }
                    },
                    label = { Text("Входящая") },
                )
                NavigationBarItem(
                    selected = tab == AppStore.Tab.SETTINGS,
                    onClick = { AppStore.selectTab(AppStore.Tab.SETTINGS) },
                    icon = { Icon(Icons.Filled.Settings, contentDescription = "Настройки") },
                    label = { Text("Ещё") },
                )
            }
        },
    ) { padding ->
        val content = when (tab) {
            AppStore.Tab.CHATS -> { @Composable { ChatsScreen() } }
            AppStore.Tab.TASKS -> { @Composable { TasksScreen() } }
            AppStore.Tab.PENDING -> { @Composable { ConfirmationsScreen() } }
            AppStore.Tab.NOTICES -> { @Composable { NotificationsScreen() } }
            AppStore.Tab.SETTINGS -> { @Composable { SettingsScreen() } }
        }
        androidx.compose.foundation.layout.Box(modifier = Modifier.padding(padding)) { content() }
    }
}
