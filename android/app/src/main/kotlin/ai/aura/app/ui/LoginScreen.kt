package ai.aura.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.GlassPanel

/**
 * Вход и регистрация. Состояния: форма входа/регистрации → код подтверждения
 * email → код 2FA (TOTP или резервный). Ошибки сервера показывает стор
 * (errorText → snackbar в MainActivity); здесь — только поля и действия.
 */
@Composable
fun LoginScreen() {
    val auth by AppStore.auth.collectAsState()
    var mode by remember { mutableStateOf(Mode.LOGIN) }
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var displayName by remember { mutableStateOf("") }
    var code by remember { mutableStateOf("") }
    var trustDevice by remember { mutableStateOf(true) }
    var serverUrl by remember { mutableStateOf(AppStore.store.serverUrl) }
    var showServer by remember { mutableStateOf(false) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text("Аура", style = MaterialTheme.typography.headlineLarge)
        Text(
            "Персональный AI-агент",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 24.dp),
        )

        when (val state = auth) {
            is AppStore.AuthState.NeedsEmailCode -> GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Подтвердите email", style = MaterialTheme.typography.titleMedium)
                    Text("Код отправлен на ${state.email}", color = MaterialTheme.colorScheme.onSurfaceVariant)
                    OutlinedTextField(
                        value = code, onValueChange = { code = it },
                        label = { Text("Код из письма") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Button(onClick = { AppStore.verifyEmail(state.email, code) },
                        modifier = Modifier.fillMaxWidth()) { Text("Подтвердить") }
                    TextButton(onClick = { AppStore.resendCode(state.email) }) {
                        Text("Отправить код ещё раз")
                    }
                    TextButton(onClick = { AppStore.logout() }) { Text("Отмена") }
                }
            }

            is AppStore.AuthState.Needs2fa -> GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Двухфакторная аутентификация", style = MaterialTheme.typography.titleMedium)
                    OutlinedTextField(
                        value = code, onValueChange = { code = it },
                        label = { Text("Код из приложения-аутентификатора") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    androidx.compose.foundation.layout.Row(
                        verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
                    ) {
                        Switch(checked = trustDevice, onCheckedChange = { trustDevice = it })
                        Text(" Доверять этому устройству",
                            modifier = Modifier.padding(start = 8.dp))
                    }
                    Button(onClick = {
                        AppStore.login2fa(code, trustDevice)
                        code = ""
                    }, modifier = Modifier.fillMaxWidth()) { Text("Подтвердить вход") }
                    Text("Подойдёт и резервный код вида XXXXX-XXXXX",
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    TextButton(onClick = { AppStore.logout() }) { Text("Отмена") }
                }
            }

            else -> GlassPanel(modifier = Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    if (mode == Mode.REGISTER) {
                        OutlinedTextField(
                            value = displayName, onValueChange = { displayName = it },
                            label = { Text("Имя") }, modifier = Modifier.fillMaxWidth(),
                        )
                    }
                    OutlinedTextField(
                        value = email, onValueChange = { email = it },
                        label = { Text("Email") },
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    OutlinedTextField(
                        value = password, onValueChange = { password = it },
                        label = { Text("Пароль") },
                        visualTransformation = PasswordVisualTransformation(),
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                        modifier = Modifier.fillMaxWidth(),
                    )
                    if (mode == Mode.LOGIN) {
                        Button(onClick = { AppStore.login(email.trim(), password) },
                            modifier = Modifier.fillMaxWidth()) { Text("Войти") }
                        TextButton(onClick = { mode = Mode.REGISTER },
                            modifier = Modifier.fillMaxWidth()) { Text("Создать аккаунт") }
                    } else {
                        Button(onClick = { AppStore.register(displayName.trim(), email.trim(), password) },
                            modifier = Modifier.fillMaxWidth()) { Text("Зарегистрироваться") }
                        TextButton(onClick = { mode = Mode.LOGIN },
                            modifier = Modifier.fillMaxWidth()) { Text("У меня уже есть аккаунт") }
                    }
                    TextButton(onClick = { showServer = !showServer },
                        modifier = Modifier.fillMaxWidth()) { Text("Адрес сервера") }
                    if (showServer) {
                        OutlinedTextField(
                            value = serverUrl, onValueChange = { serverUrl = it },
                            label = { Text("ws:// или wss://") },
                            modifier = Modifier.fillMaxWidth(),
                        )
                        OutlinedButton(onClick = { AppStore.setServerUrl(serverUrl.trim()) },
                            modifier = Modifier.fillMaxWidth()) { Text("Сохранить адрес") }
                    }
                }
            }
        }
    }
}

private enum class Mode { LOGIN, REGISTER }
