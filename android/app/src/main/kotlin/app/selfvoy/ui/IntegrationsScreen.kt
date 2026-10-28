package app.selfvoy.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.automirrored.outlined.OpenInNew
import androidx.compose.material.icons.automirrored.outlined.Send
import androidx.compose.material.icons.outlined.CalendarMonth
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.CreditCard
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material.icons.outlined.Visibility
import androidx.compose.material.icons.outlined.VisibilityOff
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import app.selfvoy.AppStore
import app.selfvoy.kit.CalendarFeed
import app.selfvoy.ui.theme.BrandAmber
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.NoticeBox
import app.selfvoy.ui.theme.ProBadge
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.SoftButton
import app.selfvoy.ui.theme.StatusIcon
import app.selfvoy.ui.theme.StatusPill
import app.selfvoy.ui.theme.StatusView
import app.selfvoy.ui.theme.SwitchRow
import app.selfvoy.ui.theme.Tint
import app.selfvoy.ui.theme.Tone
import kotlinx.coroutines.launch

/**
 * «Подключения» владельца: Telegram-бот (F2), оплата через ЮKassa (F4) и
 * подписка на календарь по ссылке (F6). Секретный ключ ЮKassa живёт только в
 * поле ввода до отправки: не сохраняется, не логируется, после подключения
 * и при уходе с экрана поле очищается.
 */
@Composable
fun IntegrationsScreen() {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 20.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item { RoundIconButton(Icons.AutoMirrored.Outlined.ArrowBack, "Назад", { AppStore.selectTab(AppStore.Tab.MORE) }, size = 38.dp) }
        item { ScreenHeader("Подключения") }
        item { TelegramSection() }
        item { CalendarFeedSection() }
        item { YooKassaSection() }
    }
}

// ------------------------------------------------------------------ Telegram

@Composable
private fun TelegramSection() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status by AppStore.telegram.collectAsState()
    val prefs by AppStore.prefs.collectAsState()
    var link by remember { mutableStateOf<app.selfvoy.kit.TelegramLink?>(null) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    // Вернулись из Telegram — проверяем, привязался ли чат.
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle, link) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME && link != null) AppStore.refreshTelegram()
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }

    val current = status
    Group(
        head = "Telegram",
        foot = if (current?.enabled == false) "" else "Новые записи, переносы, отмены, «Приду» и оплаты — сообщением в Telegram. Отключить можно и командой /stop в чате с ботом.",
    ) {
        ListRow(
            "Telegram",
            when {
                current == null -> "Проверяем…"
                !current.enabled -> "Бот не настроен на сервере"
                current.linked -> "Подключено" + if (current.username.isNotEmpty()) " · @${current.username}" else ""
                current.botUsername.isNotEmpty() -> "Бот @${current.botUsername}"
                else -> "Не подключено"
            },
            Icons.AutoMirrored.Outlined.Send, Color(0xFF2AABEE),
        ) {
            when {
                current == null -> CircularProgressIndicator(Modifier.size(20.dp), color = Tint, strokeWidth = 2.dp)
                !current.enabled -> StatusPill(StatusView("Недоступно", Tone.NEUTRAL, StatusIcon.MINUS))
                current.linked -> GlassButton("Отключить", {
                    busy = true
                    scope.launch {
                        val result = AppStore.telegramUnlink()
                        busy = false
                        error = result.error
                    }
                }, danger = true, enabled = !busy, height = 34.dp)
                else -> GradientButton("Подключить", {
                    busy = true
                    error = null
                    scope.launch {
                        val result = AppStore.telegramLink()
                        busy = false
                        val value = result.value
                        if (value != null) {
                            link = value
                            if (!openUrl(context, value.url)) error = "Не удалось открыть Telegram — откройте ссылку ниже"
                        } else {
                            error = result.error
                        }
                    }
                }, busy = busy, enabled = !busy, height = 34.dp)
            }
        }
        val pending = link
        if (pending != null && current?.linked != true) {
            GroupDivider()
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Откройте бота и нажмите «Запустить» (Start) — затем вернитесь сюда. Ссылка действует до ${whenText(pending.expiresAt)}.",
                    style = MaterialTheme.typography.bodySmall, color = Fg)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    SoftButton("Открыть Telegram", { openUrl(context, pending.url) }, icon = Icons.AutoMirrored.Outlined.OpenInNew, height = 36.dp)
                    GlassButton("Проверить", { AppStore.refreshTelegram() }, icon = Icons.Outlined.Refresh, quiet = true, height = 36.dp)
                }
            }
        }
        if (current?.linked == true) {
            GroupDivider()
            val on = app.selfvoy.kit.telegramNotificationsEnabled(prefs)
            SwitchRow("Дублировать уведомления", "Тихие часы и отключённые виды действуют и здесь", on, enabled = !busy) { value ->
                busy = true
                scope.launch {
                    val result = AppStore.setTelegramNotifications(value)
                    busy = false
                    error = result.error
                }
            }
        }
        error?.let {
            GroupDivider()
            Column(Modifier.padding(10.dp)) { NoticeBox(it, Tone.RED) }
        }
    }
}

// ------------------------------------------------------------------ календарь (ICS)

@Composable
private fun CalendarFeedSection() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val feed by AppStore.calendarFeed.collectAsState()
    var fresh by remember { mutableStateOf<String?>(null) }
    var confirming by remember { mutableStateOf<String?>(null) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }

    fun reset() {
        busy = true
        error = null
        scope.launch {
            val result = AppStore.resetCalendarFeed()
            busy = false
            if (result.ok) {
                fresh = result.value
                confirming = null
                if (result.value == null) error = "Ссылку не собрать: адрес сервера неизвестен. Задайте на сервере SELFVOY_SERVER_PUBLIC_URL."
            } else {
                error = result.error
            }
        }
    }

    val current = feed
    Group(
        head = "Календарь в телефоне",
        foot = "Встречи записей и принятые приглашения появятся в Google, Apple или Outlook Календаре — «Добавить календарь по URL». Календарь обновляется раз в час.",
    ) {
        ListRow("Подписка по ссылке", when {
            current == null -> "Проверяем…"
            current.enabled -> "Включена"
            else -> "Выключена"
        }, Icons.Outlined.CalendarMonth, Tint) {
            when {
                current == null -> CircularProgressIndicator(Modifier.size(20.dp), color = Tint, strokeWidth = 2.dp)
                current.enabled -> StatusPill(StatusView("Включена", Tone.GREEN, StatusIcon.CHECK))
                else -> GradientButton("Включить", { reset() }, busy = busy, enabled = !busy, height = 34.dp)
            }
        }
        val url = fresh
        if (url != null) {
            GroupDivider()
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Muted("Ваша ссылка — покажем её только сейчас. Не публикуйте: по ней видны ваши встречи.")
                Text(url, style = MaterialTheme.typography.bodySmall, color = Fg)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    GradientButton("Скопировать", {
                        copyText(context, url)
                        AppStore.notify("Ссылка скопирована")
                    }, Modifier.weight(1f), icon = Icons.Outlined.ContentCopy, height = 38.dp)
                    GlassButton("Поделиться", { shareText(context, "Календарь Selfvoy", url) }, Modifier.weight(1f),
                        icon = Icons.Outlined.Share, height = 38.dp)
                }
                CalendarFeed.webcal(url)?.let { webcal ->
                    SoftButton("Открыть в календаре", {
                        if (!openUrl(context, webcal)) AppStore.notify("Нет приложения календаря — скопируйте ссылку")
                    }, Modifier.fillMaxWidth(), icon = Icons.Outlined.CalendarMonth, height = 38.dp)
                }
            }
        }
        if (current?.enabled == true) {
            GroupDivider()
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (url == null) Muted("Ссылка показывается один раз. Потеряли её или хотите отключить старую — создайте новую.")
                when (confirming) {
                    "reset" -> NoticeBox("Старая ссылка перестанет работать во всех календарях, где она добавлена. Создать новую?", Tone.AMBER) {
                        GradientButton("Да, новая ссылка", { reset() }, busy = busy, enabled = !busy, height = 36.dp)
                        GlassButton("Нет", { confirming = null }, quiet = true, height = 36.dp)
                    }
                    "disable" -> NoticeBox("Календари перестанут получать ваши встречи. Выключить подписку?", Tone.RED) {
                        GlassButton("Выключить", {
                            busy = true
                            scope.launch {
                                val result = AppStore.disableCalendarFeed()
                                busy = false
                                if (result.ok) {
                                    fresh = null
                                    confirming = null
                                } else {
                                    error = result.error
                                }
                            }
                        }, danger = true, enabled = !busy, height = 36.dp)
                        GlassButton("Нет", { confirming = null }, quiet = true, height = 36.dp)
                    }
                    else -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        GlassButton("Новая ссылка", { confirming = "reset" }, icon = Icons.Outlined.Refresh, height = 36.dp)
                        GlassButton("Выключить", { confirming = "disable" }, danger = true, height = 36.dp)
                    }
                }
            }
        }
        error?.let {
            GroupDivider()
            Column(Modifier.padding(10.dp)) { NoticeBox(it, Tone.RED) }
        }
    }
}

// ------------------------------------------------------------------ ЮKassa

@Composable
private fun YooKassaSection() {
    val scope = rememberCoroutineScope()
    val status by AppStore.payments.collectAsState()
    var shopId by remember { mutableStateOf("") }
    // Ключ — только в памяти поля до отправки; rememberSaveable не используем, чтобы он не попал в сохранённое состояние.
    var secret by remember { mutableStateOf("") }
    var showSecret by remember { mutableStateOf(false) }
    var confirming by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var proError by remember { mutableStateOf(false) }

    // Уходим с экрана — ключ из памяти поля тоже уходит.
    DisposableEffect(Unit) { onDispose { secret = "" } }

    val current = status
    Group(
        head = "Оплата: ЮKassa",
        headBadge = { ProBadge() },
        foot = "Свой магазин ЮKassa: деньги клиентов идут прямо вам, Selfvoy их не касается. Ключ хранится на сервере зашифрованным и никому не показывается. Чеки — в «Мой налог» или в кабинете ЮKassa.",
    ) {
        ListRow("ЮKassa", when {
            current == null -> "Проверяем…"
            current.connected -> "Магазин ${current.shopId}" + if (current.connectedAt.isNotEmpty()) " · с ${whenText(current.connectedAt)}" else ""
            else -> "Не подключена"
        }, Icons.Outlined.CreditCard, BrandAmber) {
            when {
                current == null -> CircularProgressIndicator(Modifier.size(20.dp), color = Tint, strokeWidth = 2.dp)
                current.connected -> StatusPill(StatusView("Подключена", Tone.GREEN, StatusIcon.CHECK))
                else -> Unit
            }
        }
        if (current?.connected == true) {
            GroupDivider()
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if (confirming) {
                    NoticeBox("Отключить ЮKassa? Новые записи с предоплатой будут оплачиваться по ссылке или реквизитам страницы.", Tone.RED) {
                        GlassButton("Отключить", {
                            busy = true
                            scope.launch {
                                val result = AppStore.disconnectYooKassa()
                                busy = false
                                confirming = false
                                error = result.error
                            }
                        }, danger = true, enabled = !busy, height = 36.dp)
                        GlassButton("Нет", { confirming = false }, quiet = true, height = 36.dp)
                    }
                } else {
                    GlassButton("Отключить", { confirming = true }, danger = true, height = 36.dp)
                }
            }
        } else if (current != null) {
            GroupDivider()
            Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                SelfvoyField(shopId, { shopId = it.filter(Char::isDigit).take(20); error = null }, "shopId — цифры из кабинета ЮKassa",
                    label = "Идентификатор магазина", keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                SelfvoyField(
                    secret, { secret = it.trim(); error = null }, "live_… или test_…", label = "Секретный ключ",
                    password = !showSecret,
                    trailing = if (showSecret) Icons.Outlined.VisibilityOff else Icons.Outlined.Visibility,
                    onTrailingClick = { showSecret = !showSecret },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                )
                if (proError) ProRequiredNotice { AppStore.selectTab(AppStore.Tab.MORE) }
                GradientButton("Подключить", {
                    busy = true
                    error = null
                    proError = false
                    scope.launch {
                        val result = AppStore.connectYooKassa(shopId, secret)
                        busy = false
                        if (result.ok) {
                            secret = ""
                            shopId = ""
                        } else {
                            proError = result.proRequired
                            error = if (result.proRequired) null else result.error
                        }
                    }
                }, Modifier.fillMaxWidth(), busy = busy, enabled = !busy && shopId.isNotEmpty() && secret.isNotEmpty(), height = 44.dp)
            }
        }
        error?.let {
            GroupDivider()
            Column(Modifier.padding(10.dp)) { NoticeBox(it, Tone.RED) }
        }
    }
}
