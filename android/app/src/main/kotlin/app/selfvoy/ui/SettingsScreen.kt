package app.selfvoy.ui

import android.content.Context
import androidx.browser.customtabs.CustomTabsIntent
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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.automirrored.outlined.Logout
import androidx.compose.material.icons.outlined.CalendarMonth
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.EventAvailable
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material.icons.outlined.Mail
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.kit.SelfvoyUser
import app.selfvoy.kit.Privacy
import app.selfvoy.ui.theme.Accent
import app.selfvoy.ui.theme.AccentSoft
import app.selfvoy.ui.theme.SelfvoyAvatar
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.Danger
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.SectionTitle
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import app.selfvoy.ui.theme.Warning
import app.selfvoy.ui.theme.FillSoft
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Segmented
import app.selfvoy.ui.theme.SelfvoyPalette
import app.selfvoy.ui.theme.ThemeMode
import kotlinx.coroutines.launch

/**
 * Настройки: профиль (имя и «о себе» через profile.update), сервер, что разрешено
 * Selfvoy (allow/ask/deny по инструментам), приватность ИИ в переговорах (prefs.privacy), 2FA (setup → confirm → резервные коды, отключение по паролю), интеграции Google
 * (Custom Tabs → selfvoy://oauth), выход. Кнопки делают только то, что умеет сервер.
 */
@Composable
fun SettingsScreen() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val auth by AppStore.auth.collectAsState()
    val permissions by AppStore.permissions.collectAsState()
    val twoFaEnabled by AppStore.twoFaEnabled.collectAsState()
    val twoFaSetup by AppStore.twoFaSetup.collectAsState()
    val recoveryCodes by AppStore.recoveryCodes.collectAsState()
    val connections by AppStore.connections.collectAsState()
    val pushStatus by AppStore.pushStatus.collectAsState()
    val privacy by AppStore.privacy.collectAsState()
    val privacyError by AppStore.privacyError.collectAsState()
    val profileSaving by AppStore.profileSaving.collectAsState()
    val profileError by AppStore.profileError.collectAsState()
    val referral by AppStore.referral.collectAsState()
    val themeMode = SelfvoyPalette.mode
    var serverUrl by remember { mutableStateOf(AppStore.store.serverUrl) }
    var totpCode by remember { mutableStateOf("") }
    var disablePassword by remember { mutableStateOf("") }
    val me = auth as? AppStore.AuthState.LoggedIn
    // Черновик профиля; после сохранения/входа заново берётся из стора.
    var nameDraft by remember(me?.displayName) { mutableStateOf(me?.displayName.orEmpty()) }
    var bioDraft by remember(me?.bio) { mutableStateOf(me?.bio.orEmpty()) }
    val nameLength = nameDraft.trim().let { it.codePointCount(0, it.length) }
    val bioLength = bioDraft.trim().let { it.codePointCount(0, it.length) }
    val nameError = when {
        nameLength == 0 -> "Имя не может быть пустым"
        nameLength > SelfvoyUser.DISPLAY_NAME_MAX -> "Имя — до ${SelfvoyUser.DISPLAY_NAME_MAX} символов"
        else -> null
    }
    val bioError = if (bioLength > SelfvoyUser.BIO_MAX) "«О себе» — до ${SelfvoyUser.BIO_MAX} символов" else null
    val nameChanged = me != null && nameDraft.trim() != me.displayName
    val bioChanged = me != null && bioDraft.trim() != me.bio

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item { RoundIconButton(Icons.AutoMirrored.Outlined.ArrowBack, "Назад", { AppStore.selectTab(AppStore.Tab.MORE) }, size = 38.dp) }
        item { ScreenHeader("Настройки") }

        item {
            GlassCard(Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        SelfvoyAvatar(me?.displayName.orEmpty(), size = 52.dp, online = true)
                        Column(Modifier.weight(1f).padding(start = 14.dp)) {
                            Text(me?.displayName.orEmpty(), style = MaterialTheme.typography.titleLarge, color = TextPrimary)
                            Text(me?.email.orEmpty(), style = MaterialTheme.typography.bodySmall, color = TextSecondary)
                            Muted(pushStatus)
                        }
                    }
                    SelfvoyField(nameDraft, { nameDraft = it; AppStore.clearProfileError() }, "Как вас зовут", label = "Имя")
                    if (nameError != null) Text(nameError, style = MaterialTheme.typography.bodySmall, color = Danger)
                    SelfvoyField(bioDraft, { bioDraft = it; AppStore.clearProfileError() }, "Пара слов о себе — увидят ваши контакты",
                        label = "О себе", maxLines = 5)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(bioError.orEmpty(), style = MaterialTheme.typography.bodySmall, color = Danger, modifier = Modifier.weight(1f))
                        Text("$bioLength/${SelfvoyUser.BIO_MAX}", style = MaterialTheme.typography.labelSmall,
                            color = if (bioError != null) Danger else TextSecondary)
                    }
                    profileError?.let { Text(it, style = MaterialTheme.typography.bodySmall, color = Danger) }
                    GradientButton(
                        "Сохранить профиль",
                        {
                            AppStore.saveProfile(
                                displayName = nameDraft.trim().takeIf { nameChanged },
                                bio = bioDraft.trim().takeIf { bioChanged },
                            )
                        },
                        Modifier.fillMaxWidth(), height = 44.dp, busy = profileSaving,
                        enabled = nameError == null && bioError == null && (nameChanged || bioChanged) && !profileSaving,
                    )
                }
            }
        }

        item {
            Group(head = "Оформление") {
                Column(Modifier.padding(14.dp)) {
                    Segmented(ThemeMode.entries.map { it.key to it.label }, themeMode.key) { key ->
                        AppStore.setThemeMode(ThemeMode.of(key))
                    }
                }
            }
        }

        item {
            Group {
                ListRow("Страницы записи", icon = Icons.Outlined.EventAvailable, iconBackground = Accent,
                    onClick = { AppStore.selectTab(AppStore.Tab.BOOKING) })
                GroupDivider()
                ListRow("Подключения", "Telegram, ЮKassa, календарь в телефоне", Icons.Outlined.CalendarMonth, Accent,
                    onClick = { AppStore.selectTab(AppStore.Tab.INTEGRATIONS) })
            }
        }

        item { SectionTitle("Пригласить друзей") }
        item { ReferralCard(referral) }

        item { SectionTitle("Что может ваш Selfvoy") }
        item { Muted("Selfvoy лишь формирует намерение — исполняет сервер. Опасные действия по умолчанию ждут вашего подтверждения.") }
        items(permissions, key = { it.tool }) { row ->
            GlassCard(Modifier.fillMaxWidth(), contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp)) {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(toolLabel(row.tool), style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                        if (row.dangerous) {
                            Spacer(Modifier.width(8.dp))
                            Box(Modifier.clip(RoundedCornerShape(50)).border(1.dp, Warning.copy(alpha = 0.5f), RoundedCornerShape(50))
                                .padding(horizontal = 8.dp, vertical = 2.dp)) {
                                Text("опасный", style = MaterialTheme.typography.labelSmall, color = Warning)
                            }
                        }
                    }
                    Muted(toolDescription(row.tool, row.description))
                    PermissionModes(row.mode) { AppStore.setPermission(row.tool, it) }
                }
            }
        }

        item { SectionTitle("Приватность ИИ") }
        item { Muted("Что ваш Selfvoy раскрывает чужому Selfvoy, когда договаривается о встрече.") }
        item {
            GlassCard(Modifier.fillMaxWidth(), contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp)) {
                Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    val current = privacy
                    if (current == null) Muted("Загружаем настройки приватности…")
                    val shown = current ?: Privacy()
                    PrivacySwitch(
                        "Делиться только свободными слотами",
                        "Только занято/свободно и нужные для встречи настройки — без фактов памяти, дня рождения и аллергий.",
                        shown.shareFreeSlotsOnly, enabled = current != null,
                    ) { AppStore.setPrivacyFlag(Privacy.SHARE_FREE_SLOTS_ONLY, it) }
                    PrivacySwitch(
                        "Разрешить выбор локаций",
                        "Если выключено у вас или у собеседника, Selfvoy не предлагает место.",
                        shown.allowPlaceChoice, enabled = current != null,
                    ) { AppStore.setPrivacyFlag(Privacy.ALLOW_PLACE_CHOICE, it) }
                    PrivacySwitch(
                        "Скрывать названия других событий",
                        "Названия событий вашего календаря собеседник видит как «занято».",
                        shown.hideEventTitles, enabled = current != null,
                    ) { AppStore.setPrivacyFlag(Privacy.HIDE_EVENT_TITLES, it) }
                    privacyError?.let { Text("Не сохранилось: $it", style = MaterialTheme.typography.bodySmall, color = Danger) }
                }
            }
        }

        item { SectionTitle("Безопасность") }
        item {
            GlassCard(Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("Двухфакторная аутентификация", style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                    Text(if (twoFaEnabled) "Включена" else "Выключена", color = if (twoFaEnabled) AccentSoft else TextSecondary,
                        style = MaterialTheme.typography.bodySmall)
                    if (!twoFaEnabled && twoFaSetup == null) {
                        GradientButton("Включить 2FA", { AppStore.setup2fa() }, Modifier.fillMaxWidth(), height = 44.dp)
                    }
                    twoFaSetup?.let { setup ->
                        Muted("Добавьте секрет в приложение-аутентификатор:")
                        Text(setup.otpauthUri.ifEmpty { setup.secret }, color = AccentSoft, style = MaterialTheme.typography.bodySmall)
                        SelfvoyField(totpCode, { totpCode = it }, "Код из приложения")
                        GradientButton("Подтвердить и включить", { AppStore.confirm2fa(totpCode); totpCode = "" },
                            Modifier.fillMaxWidth(), height = 44.dp)
                    }
                    if (recoveryCodes.isNotEmpty()) {
                        Muted("Резервные коды (сохраните, показываются один раз):")
                        recoveryCodes.forEach { Text(it, color = AccentSoft, style = MaterialTheme.typography.labelMedium) }
                    }
                    if (twoFaEnabled) {
                        SelfvoyField(disablePassword, { disablePassword = it }, "Пароль", password = true)
                        GlassButton("Отключить 2FA", { AppStore.disable2fa(disablePassword); disablePassword = "" },
                            Modifier.fillMaxWidth(), danger = true)
                    }
                }
            }
        }

        item { SectionTitle("Интеграции") }
        item {
            GlassCard(Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Muted("Google Календарь и Gmail через официальный OAuth. Токены хранятся только на сервере.")
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        GlassButton("Календарь", { openOAuth(context, scope, "google_calendar") }, Modifier.weight(1f),
                            icon = Icons.Outlined.CalendarMonth)
                        GlassButton("Gmail", { openOAuth(context, scope, "google_gmail") }, Modifier.weight(1f),
                            icon = Icons.Outlined.Mail)
                    }
                    if (connections.isEmpty()) Muted("Подключений нет")
                    connections.forEach { connection ->
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("${connection.provider} · ${connection.account.ifEmpty { connection.status }}",
                                style = MaterialTheme.typography.bodySmall, color = TextPrimary, modifier = Modifier.weight(1f))
                            GlassButton("Синхр.", { AppStore.syncIntegration(connection.id) }, height = 34.dp, quiet = true)
                            GlassButton("Отозвать", { AppStore.revokeIntegration(connection.id) }, height = 34.dp, quiet = true)
                        }
                    }
                }
            }
        }

        item { SectionTitle("Сервер") }
        item {
            GlassCard(Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    SelfvoyField(serverUrl, { serverUrl = it }, "ws:// или wss://")
                    GlassButton("Сохранить и переподключиться", { AppStore.setServerUrl(serverUrl.trim()) }, Modifier.fillMaxWidth())
                }
            }
        }
        item {
            GlassButton("Выйти", { AppStore.logout() }, Modifier.fillMaxWidth(), icon = Icons.AutoMirrored.Outlined.Logout,
                danger = true, height = 50.dp)
        }
    }
}

/** Реферальный код и ссылка: друг заводит Selfvoy — регистрация засчитывается после подтверждения email. */
@Composable
private fun ReferralCard(info: app.selfvoy.kit.ReferralInfo?) {
    val context = LocalContext.current
    GlassCard(Modifier.fillMaxWidth(), contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            if (info == null) Muted("Загружаем реферальный код…") else {
            Muted("Друг заведёт свой Selfvoy — и ваши Selfvoy смогут договариваться о встречах напрямую. Регистрации со страниц записи тоже засчитываются вам.")
            Row(verticalAlignment = Alignment.CenterVertically) {
                Muted("Ваш код", Modifier.weight(1f))
                Text(info.code, style = MaterialTheme.typography.titleLarge, color = TextPrimary)
            }
            if (info.hasLink) Text(info.link, style = MaterialTheme.typography.bodySmall, color = TextPrimary)
            else Muted("Сервер не знает свой публичный адрес (SELFVOY_PUBLIC_URL) — делимся только кодом: друг введёт его при регистрации.")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GlassButton("Копировать", { copyText(context, info.link) }, Modifier.weight(1f), icon = Icons.Outlined.ContentCopy, height = 40.dp)
                GlassButton("Поделиться", { shareText(context, "Selfvoy", info.shareText) }, Modifier.weight(1f),
                    icon = Icons.Outlined.Share, height = 40.dp)
            }
            Muted("Зарегистрировались: ${info.joined} · подтвердили email: ${info.verified}")
            info.referrals.forEach { item ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(item.displayName.ifEmpty { "Без имени" }, style = MaterialTheme.typography.bodyMedium, color = TextPrimary,
                        modifier = Modifier.weight(1f))
                    Muted(if (item.verified) "С Selfvoy" else "Ждёт подтверждения")
                }
            }
            }
        }
    }
}

/** Флаг приватности: подпись, пояснение в одну строку и переключатель (сохраняется сразу). */
@Composable
private fun PrivacySwitch(title: String, hint: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(end = 12.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = TextPrimary)
            Muted(hint)
        }
        Switch(checked = checked, onCheckedChange = onChange, enabled = enabled)
    }
}

/** Три режима инструмента: сам / с подтверждения / запрещено. */
@Composable
private fun PermissionModes(mode: String, onSelect: (String) -> Unit) {
    val shape = RoundedCornerShape(50)
    Row(Modifier.fillMaxWidth().clip(shape).background(FillSoft).padding(3.dp)) {
        listOf("allow" to "Сам", "ask" to "Спрашивать", "deny" to "Запретить").forEach { (value, label) ->
            val active = mode == value
            Box(
                Modifier.weight(1f).clip(shape)
                    .background(if (active) (if (value == "deny") Danger.copy(alpha = 0.18f) else Accent.copy(alpha = 0.16f)) else Color.Transparent)
                    .clickable { if (!active) onSelect(value) }.padding(vertical = 8.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(label, style = MaterialTheme.typography.labelMedium, color = if (active) (if (value == "deny") Danger else Accent) else TextSecondary,
                    textAlign = TextAlign.Center)
            }
        }
    }
}

/** Google OAuth: begin → Custom Tabs → редирект selfvoy://oauth (MainActivity). */
private fun openOAuth(context: Context, scope: kotlinx.coroutines.CoroutineScope, provider: String) {
    scope.launch {
        val url = AppStore.beginIntegration(provider)
        if (url.isNotEmpty()) CustomTabsIntent.Builder().build().launchUrl(context, android.net.Uri.parse(url))
    }
}
