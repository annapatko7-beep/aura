package app.selfvoy.ui

import androidx.compose.foundation.Canvas
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
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Lock
import androidx.compose.material.icons.outlined.Mail
import androidx.compose.material.icons.outlined.Schedule
import androidx.compose.material.icons.outlined.Visibility
import androidx.compose.material.icons.outlined.VisibilityOff
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.SelfvoyBackground
import app.selfvoy.ui.theme.SelfvoyEmblem
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import app.selfvoy.kit.ReferralCode
import java.time.ZoneId

/**
 * Вход (макет Welcome) и регистрация (макет Create your Selfvoy), плюс шаги auth v3:
 * код из письма, 2FA (TOTP или резервный код), сброс пароля. Ошибки сервера
 * показывает стор (errorText → snackbar в MainActivity).
 */
@Composable
fun LoginScreen() {
    val auth by AppStore.auth.collectAsState()
    val busy by AppStore.busy.collectAsState()
    var register by remember { mutableStateOf(false) }
    var email by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }
    var displayName by remember { mutableStateOf("") }
    var focus by remember { mutableStateOf("") }
    var refCode by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var code by remember { mutableStateOf("") }
    var newPassword by remember { mutableStateOf("") }
    var trustDevice by remember { mutableStateOf(true) }
    var serverUrl by remember { mutableStateOf(AppStore.store.serverUrl) }
    var showServer by remember { mutableStateOf(false) }
    val signupRef by AppStore.signupRef.collectAsState()

    // selfvoy://signup?ref=CODE — сразу форма регистрации с подставленным кодом.
    LaunchedEffect(signupRef) {
        val link = signupRef ?: return@LaunchedEffect
        register = true
        if (link.isNotEmpty()) refCode = link
        AppStore.consumeSignupRef()
    }

    SelfvoyBackground {
        Column(
            Modifier.fillMaxSize().statusBarsPadding().imePadding().verticalScroll(rememberScrollState()).padding(20.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            GlassCard(Modifier.fillMaxWidth(), radius = 34.dp, contentPadding = androidx.compose.foundation.layout.PaddingValues(26.dp)) {
                Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    val state = auth
                    val isRegister = register && state is AppStore.AuthState.LoggedOut
                    if (isRegister) {
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                            SelfvoyEmblem(size = 28.dp)
                            Spacer(Modifier.width(10.dp))
                            Text("Selfvoy", style = MaterialTheme.typography.titleLarge, color = TextPrimary)
                        }
                    } else {
                        SelfvoyEmblem(76.dp, Modifier.align(Alignment.CenterHorizontally))
                    }
                    Text(
                        when (state) {
                            is AppStore.AuthState.NeedsEmailCode -> "Подтверждение email"
                            is AppStore.AuthState.Needs2fa -> "Двухфакторный вход"
                            is AppStore.AuthState.NeedsResetCode -> "Новый пароль"
                            else -> if (isRegister) "Создайте свой Selfvoy" else "Добро пожаловать в Selfvoy"
                        },
                        style = MaterialTheme.typography.headlineMedium, color = TextPrimary,
                        textAlign = if (isRegister) TextAlign.Start else TextAlign.Center, modifier = Modifier.fillMaxWidth(),
                    )

                    when (state) {
                        is AppStore.AuthState.NeedsEmailCode -> {
                            Muted("Код отправлен на ${state.email}", Modifier.fillMaxWidth(), align = TextAlign.Center)
                            SelfvoyField(code, { code = it }, "6 цифр", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                            GradientButton("Подтвердить", { AppStore.verifyEmail(state.email, code); code = "" }, Modifier.fillMaxWidth(), busy = busy)
                            Row {
                                Link("Отправить код ещё раз") { AppStore.resendCode(state.email) }
                                Spacer(Modifier.weight(1f))
                                Link("Назад ко входу") { AppStore.cancelAuthFlow() }
                            }
                        }
                        is AppStore.AuthState.Needs2fa -> {
                            Muted("Код из приложения-аутентификатора или резервный код XXXXX-XXXXX", Modifier.fillMaxWidth(), align = TextAlign.Center)
                            SelfvoyField(code, { code = it }, "123456", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Switch(checked = trustDevice, onCheckedChange = { trustDevice = it })
                                Text("  Доверять устройству 90 дней", style = MaterialTheme.typography.bodySmall, color = TextSecondary)
                            }
                            GradientButton("Войти", { AppStore.login2fa(code, trustDevice); code = "" }, Modifier.fillMaxWidth(), busy = busy)
                            Link("Отмена", Modifier.align(Alignment.CenterHorizontally)) { AppStore.logout() }
                        }
                        is AppStore.AuthState.NeedsResetCode -> {
                            Muted("Код сброса отправлен на ${state.email}", Modifier.fillMaxWidth(), align = TextAlign.Center)
                            SelfvoyField(code, { code = it }, "Код из письма", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                            SelfvoyField(newPassword, { newPassword = it }, "Новый пароль: 8+ символов, буквы и цифры", password = true,
                                trailing = Icons.Outlined.Lock)
                            GradientButton("Сменить пароль", { AppStore.resetPassword(state.email, code, newPassword); code = ""; newPassword = "" },
                                Modifier.fillMaxWidth(), busy = busy)
                            Link("Назад ко входу", Modifier.align(Alignment.CenterHorizontally)) { AppStore.cancelAuthFlow() }
                        }
                        else -> if (isRegister) {
                            SelfvoyField(displayName, { displayName = it }, "Введите имя и фамилию", label = "Полное имя")
                            SelfvoyField(email, { email = it }, "Введите email", label = "Email",
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email))
                            SelfvoyField(password, { password = it }, "Минимум 8 символов, буквы и цифры", label = "Пароль",
                                password = !showPassword,
                                trailing = if (showPassword) Icons.Outlined.VisibilityOff else Icons.Outlined.Visibility,
                                onTrailingClick = { showPassword = !showPassword })
                            SelfvoyField(ZoneId.systemDefault().id + " · по системе", {}, "", label = "Часовой пояс", readOnly = true,
                                trailing = Icons.Outlined.Schedule)
                            SelfvoyField(focus, { focus = it }, "например, встречи, продуктивность, код",
                                label = "С чем должен помогать ваш Selfvoy?", filled = true)
                            SelfvoyField(refCode, { refCode = it.trim().uppercase().take(ReferralCode.LENGTH) }, "Например, ABCD2345",
                                label = "Код приглашения (необязательно)",
                                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Characters))
                            if (refCode.isNotEmpty() && ReferralCode.normalize(refCode) == null) {
                                Muted("Код — ${ReferralCode.LENGTH} латинских букв и цифр; с неверным кодом регистрация пройдёт без приглашения")
                            }
                            GradientButton("Создать аккаунт", { AppStore.register(displayName.trim(), email.trim(), password, focus, refCode) },
                                Modifier.fillMaxWidth().padding(top = 6.dp), busy = busy)
                            Muted("Пароль — Argon2id, сессия — JWT", Modifier.fillMaxWidth(), align = TextAlign.Center)
                            Link("Уже есть аккаунт?", Modifier.align(Alignment.End)) { register = false }
                        } else {
                            Muted("Ваша персональная сеть ИИ-агентов", Modifier.fillMaxWidth(), align = TextAlign.Center)
                            SelfvoyField(email, { email = it }, "Введите email", label = "Email", trailing = Icons.Outlined.Mail,
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email))
                            SelfvoyField(password, { password = it }, "••••••••", label = "Пароль", password = true,
                                trailing = Icons.Outlined.Lock, onDone = { AppStore.login(email.trim(), password) })
                            Link("Забыли пароль?", Modifier.align(Alignment.End)) { AppStore.forgotPassword(email.trim()) }
                            GradientButton("Войти", { AppStore.login(email.trim(), password) }, Modifier.fillMaxWidth(), busy = busy)
                            // Вход через Google сервер пока не поддерживает — кнопка честно неактивна.
                            GlassButton("Продолжить с Google", {}, Modifier.fillMaxWidth(), enabled = false)
                            Link("Создать аккаунт", Modifier.align(Alignment.End)) { register = true }
                        }
                    }
                }
            }

            Spacer(Modifier.height(16.dp))
            Link(if (showServer) "Скрыть адрес сервера" else "Адрес сервера") { showServer = !showServer }
            if (showServer) {
                Column(Modifier.fillMaxWidth().padding(top = 10.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    SelfvoyField(serverUrl, { serverUrl = it }, "ws:// или wss://")
                    GlassButton("Сохранить адрес", { AppStore.setServerUrl(serverUrl.trim()) }, Modifier.fillMaxWidth())
                    Muted("Сейчас: ${AppStore.store.serverUrl}", Modifier.fillMaxWidth(), align = TextAlign.Center)
                }
            }
        }
    }
}

@Composable
private fun Link(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Box(modifier.padding(vertical = 4.dp)) {
        Text(text, style = MaterialTheme.typography.labelMedium, color = TextPrimary,
            modifier = Modifier.clickable(onClick = onClick))
    }
}
