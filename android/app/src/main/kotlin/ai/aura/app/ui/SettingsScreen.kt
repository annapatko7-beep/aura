package ai.aura.app.ui

import android.content.Context
import androidx.browser.customtabs.CustomTabsIntent
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
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.AccentSoft
import ai.aura.app.ui.theme.GlassPanel
import kotlinx.coroutines.launch

/**
 * Настройки: аккаунт, сервер, 2FA (setup → confirm → recovery-коды, отключение
 * по паролю), интеграции Google (Custom Tabs → aura://oauth), push-статус,
 * выход. Кнопки делают только то, что умеет сервер (docs/PROTOCOL.md).
 */
@Composable
fun SettingsScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val auth by AppStore.auth.collectAsState()
    val twoFaEnabled by AppStore.twoFaEnabled.collectAsState()
    val twoFaSetup by AppStore.twoFaSetup.collectAsState()
    val recoveryCodes by AppStore.recoveryCodes.collectAsState()
    val connections by AppStore.connections.collectAsState()
    val pushStatus by AppStore.pushStatus.collectAsState()

    var serverUrl by remember { mutableStateOf(AppStore.store.serverUrl) }
    var totpCode by remember { mutableStateOf("") }
    var disablePassword by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier
            .fillMaxSize()
            .padding(horizontal = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item {
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column {
                    Text("Аккаунт", style = MaterialTheme.typography.titleMedium)
                    val current = auth as? AppStore.AuthState.LoggedIn
                    Text(current?.let { "${it.displayName} · ${it.email}" } ?: "—",
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(pushStatus, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        item {
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Сервер", style = MaterialTheme.typography.titleMedium)
                    OutlinedTextField(
                        value = serverUrl, onValueChange = { serverUrl = it },
                        label = { Text("ws:// или wss://") },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedButton(onClick = { AppStore.setServerUrl(serverUrl.trim()) }) {
                        Text("Сохранить и переподключиться")
                    }
                }
            }
        }
        item {
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Двухфакторная аутентификация",
                        style = MaterialTheme.typography.titleMedium)
                    Text(if (twoFaEnabled) "Включена" else "Выключена",
                        color = if (twoFaEnabled) AccentSoft else MaterialTheme.colorScheme.onSurfaceVariant)
                    if (!twoFaEnabled && twoFaSetup == null) {
                        Button(onClick = { AppStore.setup2fa() }) { Text("Настроить 2FA") }
                    }
                    twoFaSetup?.let { setup ->
                        Text("Добавьте секрет в приложение-аутентификатор:",
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Text(setup.otpauthUri.ifEmpty { setup.secret }, color = AccentSoft)
                        OutlinedTextField(
                            value = totpCode, onValueChange = { totpCode = it },
                            label = { Text("Код из приложения") },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Button(onClick = {
                            AppStore.confirm2fa(totpCode)
                            totpCode = ""
                        }) { Text("Подтвердить и включить") }
                    }
                    if (recoveryCodes.isNotEmpty()) {
                        Text("Резервные коды (сохраните, показываются один раз):",
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        recoveryCodes.forEach { Text(it, color = AccentSoft) }
                    }
                    if (twoFaEnabled) {
                        OutlinedTextField(
                            value = disablePassword, onValueChange = { disablePassword = it },
                            label = { Text("Пароль") },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        OutlinedButton(onClick = {
                            AppStore.disable2fa(disablePassword)
                            disablePassword = ""
                        }) { Text("Отключить 2FA") }
                    }
                }
            }
        }
        item {
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Интеграции Google", style = MaterialTheme.typography.titleMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(onClick = { openOAuth(context, scope, "google_calendar") }) {
                            Text("Календарь")
                        }
                        Button(onClick = { openOAuth(context, scope, "google_gmail") }) {
                            Text("Gmail")
                        }
                    }
                    if (connections.isEmpty()) {
                        Text("Подключений нет", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
        items(connections, key = { it.id }) { connection ->
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Text("${connection.provider} · ${connection.account.ifEmpty { connection.status }}")
                    Row {
                        TextButton(onClick = { AppStore.syncIntegration(connection.id) }) {
                            Text("Синхронизировать")
                        }
                        TextButton(onClick = { AppStore.revokeIntegration(connection.id) }) {
                            Text("Отозвать")
                        }
                    }
                }
            }
        }
        item {
            GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column {
                    Button(onClick = { AppStore.logout() }, modifier = Modifier.fillMaxWidth()) {
                        Text("Выйти")
                    }
                }
            }
        }
    }
}

/** Google OAuth: begin → Custom Tabs → редирект aura://oauth (MainActivity). */
private fun openOAuth(
    context: Context,
    scope: kotlinx.coroutines.CoroutineScope,
    provider: String,
) {
    scope.launch {
        val url = AppStore.beginIntegration(provider)
        if (url.isNotEmpty()) CustomTabsIntent.Builder().build().launchUrl(context, android.net.Uri.parse(url))
    }
}
