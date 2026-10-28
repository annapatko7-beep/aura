package app.selfvoy.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.Pause
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material.icons.outlined.Schedule
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.kit.Booking
import app.selfvoy.kit.BookingPage
import app.selfvoy.kit.BookingPageFields
import app.selfvoy.kit.BookingPolicy
import app.selfvoy.kit.Money
import app.selfvoy.kit.PackageOffer
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.InsetSheet
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.NoticeBox
import app.selfvoy.ui.theme.ProBadge
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.SectionTitle
import app.selfvoy.ui.theme.Segmented
import app.selfvoy.ui.theme.SelfvoyChip
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.SoftButton
import app.selfvoy.ui.theme.StatusIcon
import app.selfvoy.ui.theme.StatusPill
import app.selfvoy.ui.theme.StatusView
import app.selfvoy.ui.theme.SwitchRow
import app.selfvoy.ui.theme.Tone
import kotlinx.coroutines.launch
import java.time.LocalDateTime

/**
 * «Запись» — страницы записи владельца (booking.*): постоянная ссылка #/b/<slug>,
 * по которой клиенты сами выбирают свободное время (страница гостя — в
 * веб-клиенте). Здесь — создать и настроить страницу (цена, предоплата, политика
 * отмены, напоминания, подпись, пакеты), поделиться ссылкой или постом со
 * свободными окнами, увидеть записи клиентов и открыть карточку записи.
 */
@Composable
fun BookingScreen() {
    val pages by AppStore.bookingPages.collectAsState()
    val bookings by AppStore.bookings.collectAsState()
    val pending by AppStore.pendingAction.collectAsState()
    var editing by remember { mutableStateOf<BookingPage?>(null) }
    var creating by remember { mutableStateOf(false) }
    var posting by remember { mutableStateOf<Long?>(null) }
    var postOpen by remember { mutableStateOf(false) }
    var opened by remember { mutableStateOf<Long?>(null) }
    var filter by remember { mutableStateOf("upcoming") }

    // Действие из «+» или с «Сегодня».
    LaunchedEffect(pending) {
        when (pending) {
            AppStore.PendingAction.NEW_PAGE -> { creating = true; AppStore.consumePendingAction() }
            AppStore.PendingAction.SLOTS_POST -> { posting = null; postOpen = true; AppStore.consumePendingAction() }
            else -> Unit
        }
    }

    val now = LocalDateTime.now()
    val shown = bookings.filter { booking ->
        filter == "all" || (booking.active && booking.occurrences.any { it.active && (parseInstant(it.end.ifEmpty { it.start })?.isAfter(now) == true) })
    }

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            ScreenHeader("Запись") {
                GlassButton("Новая", { creating = true }, icon = Icons.Outlined.Add, height = 38.dp)
            }
        }
        item { Muted("Постоянная ссылка: клиенты без Selfvoy сами выбирают свободное время в вашем расписании — можно сразу серией раз в неделю.") }

        item { SectionTitle("Страницы") }
        val list = pages
        when {
            list == null -> item { Muted("Загружаем…") }
            list.isEmpty() -> item {
                GlassCard(Modifier.fillMaxWidth()) {
                    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text("Страниц пока нет", style = MaterialTheme.typography.titleMedium, color = Fg)
                        Muted("Создайте первую — например, «Урок, 60 минут» — и отправьте ссылку клиентам.")
                        GradientButton("Создать страницу", { creating = true }, height = 42.dp)
                    }
                }
            }
            else -> items(list, key = { "page-${it.id}" }) { page ->
                PageCard(page, onEdit = { editing = page }, onPost = { posting = page.id; postOpen = true })
            }
        }

        item { SectionTitle("Записи клиентов", Modifier.padding(top = 6.dp)) }
        item { Segmented(listOf("upcoming" to "Предстоящие", "all" to "Все"), filter) { filter = it } }
        if (shown.isEmpty()) {
            item { Muted(if (filter == "upcoming") "Предстоящих записей нет." else "Здесь появятся люди, которые записались по вашим ссылкам.") }
        } else {
            item {
                Group {
                    shown.forEachIndexed { index, booking ->
                        if (index > 0) GroupDivider()
                        BookingRow(booking) { opened = booking.id }
                    }
                }
            }
        }
    }

    if (creating || editing != null) {
        PageEditorSheet(editing) { creating = false; editing = null }
    }
    if (postOpen) SlotsPostSheet(pages.orEmpty(), posting) { postOpen = false; posting = null }
    opened?.let { id -> bookings.firstOrNull { it.id == id } }?.let { booking ->
        BookingSheet(booking, null, pages) { opened = null }
    }
}

@Composable
private fun PageCard(page: BookingPage, onEdit: () -> Unit, onPost: () -> Unit) {
    val context = LocalContext.current
    var confirmDelete by remember { mutableStateOf(false) }
    val facts = listOfNotNull(
        "${page.durationMinutes} мин",
        if (page.price > 0) Money.format(page.price, page.currency) else null,
        when (page.prepay) {
            BookingPolicy.PREPAY_FULL -> "предоплата 100%"
            BookingPolicy.PREPAY_DEPOSIT -> "депозит ${Money.format(page.deposit, page.currency)}"
            else -> null
        },
        if (page.packages.isNotEmpty()) "пакетов: ${page.packages.size}" else null,
        "на ${page.daysAhead} ${plural(page.daysAhead.toLong(), "день", "дня", "дней")} вперёд",
        if (page.maxSeries > 1) "серия до ${page.maxSeries}" else null,
        if (page.weekends) "и в выходные" else "будни",
    )
    GlassCard(Modifier.fillMaxWidth()) {
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(page.title, style = MaterialTheme.typography.titleMedium, color = Fg, modifier = Modifier.weight(1f))
                StatusPill(if (page.active) StatusView("Открыта", Tone.GREEN, StatusIcon.CHECK) else StatusView("На паузе", Tone.AMBER, StatusIcon.BLOCK))
            }
            Muted(facts.joinToString(" · "))
            Muted(if (page.upcoming > 0) "Предстоит: ${page.upcoming} ${plural(page.upcoming.toLong(), "встреча", "встречи", "встреч")}" else "Пока никто не записался")
            Text(page.link, style = MaterialTheme.typography.bodySmall, color = Fg, maxLines = 1)
            if (page.url.isEmpty()) Muted("Сервер не знает свой публичный адрес (SELFVOY_PUBLIC_URL) — полную ссылку соберите из адреса веб-клиента.")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GlassButton("Скопировать", {
                    copyText(context, page.link)
                    AppStore.notify("Ссылка скопирована")
                }, Modifier.weight(1f), icon = Icons.Outlined.ContentCopy, height = 40.dp)
                GlassButton("Поделиться", { shareText(context, page.title, page.link) }, Modifier.weight(1f), icon = Icons.Outlined.Share, height = 40.dp)
            }
            if (page.active) SoftButton("Пост со свободными окнами", onPost, Modifier.fillMaxWidth(), icon = Icons.Outlined.Schedule, height = 40.dp)
            if (confirmDelete) {
                NoticeBox("Удалить страницу? Ссылка перестанет работать, сделанные записи останутся.", Tone.RED) {
                    GlassButton("Удалить", { AppStore.deleteBookingPage(page); confirmDelete = false }, danger = true, height = 36.dp)
                    GlassButton("Нет", { confirmDelete = false }, quiet = true, height = 36.dp)
                }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    GlassButton("Настроить", onEdit, Modifier.weight(1f), icon = Icons.Outlined.Edit, quiet = true, height = 38.dp)
                    if (page.active) {
                        GlassButton("Пауза", { AppStore.setBookingPagePaused(page, true) }, Modifier.weight(1f),
                            icon = Icons.Outlined.Pause, quiet = true, height = 38.dp)
                    } else {
                        GlassButton("Открыть", { AppStore.setBookingPagePaused(page, false) }, Modifier.weight(1f),
                            icon = Icons.Outlined.PlayArrow, quiet = true, height = 38.dp)
                    }
                    GlassButton("", { confirmDelete = true }, icon = Icons.Outlined.Delete, danger = true, height = 38.dp)
                }
            }
        }
    }
}

@Composable
private fun BookingRow(booking: Booking, onOpen: () -> Unit) {
    val now = LocalDateTime.now()
    val upcoming = booking.occurrences.filter { it.active && (parseInstant(it.start)?.isAfter(now) == true) }
    val focus = upcoming.firstOrNull() ?: booking.occurrences.lastOrNull()
    val status: StatusView = when {
        !booking.active -> StatusView(
            when (booking.cancelledBy) {
                "owner" -> "Отменили вы"
                "system" -> "Не оплачено"
                else -> "Отменил гость"
            }, Tone.RED, StatusIcon.X,
        )
        focus != null && upcoming.isNotEmpty() -> occurrenceStatus(booking, focus, now)
        else -> StatusView("Прошла", Tone.NEUTRAL, StatusIcon.CHECK)
    }
    val whenLine = (upcoming.ifEmpty { booking.occurrences }).take(2).joinToString(" · ") { whenText(it.start) } +
        if (upcoming.size > 2) " и ещё ${upcoming.size - 2}" else ""
    ListRow(
        title = booking.guestName.ifEmpty { "Гость" },
        subtitle = "${booking.pageTitle} · $whenLine",
        onClick = onOpen,
    ) {
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(4.dp)) {
            StatusPill(status)
            val pay = booking.payment?.let { paymentStatusView(it.status) }
            if (pay != null && pay.label != status.label && booking.active) StatusPill(pay)
        }
    }
}

// ------------------------------------------------------------------ настройки страницы

/** Черновик предложения пакета в форме (строки — как ввёл владелец). */
private class OfferDraft(val id: Long, title: String, sessions: String, price: String, days: String) {
    var title by mutableStateOf(title)
    var sessions by mutableStateOf(sessions)
    var price by mutableStateOf(price)
    var days by mutableStateOf(days)
}

/**
 * Настройки страницы — окно снизу со сгруппированными списками: услуга,
 * расписание, цена и оплата (предоплата — Pro), отмена и перенос, клиенту
 * (напоминания, скрыть подпись — Pro), пакеты (Pro).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PageEditorSheet(page: BookingPage?, onClose: () -> Unit) {
    val scope = rememberCoroutineScope()
    val plan by AppStore.plan.collectAsState()
    val payments by AppStore.payments.collectAsState()
    LaunchedEffect(Unit) { AppStore.refreshPayments() }
    val proLocked = plan?.proLocked() == true
    val start = page?.fields ?: BookingPageFields("")

    var title by remember { mutableStateOf(start.title) }
    var description by remember { mutableStateOf(start.description) }
    var duration by remember { mutableStateOf(start.durationMinutes) }
    var days by remember { mutableStateOf(start.daysAhead) }
    var perDay by remember { mutableStateOf(start.maxPerDay.toString()) }
    var notice by remember { mutableStateOf(start.noticeHours.toString()) }
    var series by remember { mutableStateOf(start.maxSeries.toString()) }
    var weekends by remember { mutableStateOf(start.weekends) }
    var reminders by remember { mutableStateOf(start.guestReminders) }
    var hideBranding by remember { mutableStateOf(start.hideBranding) }
    var price by remember { mutableStateOf(Money.inputValue(start.price)) }
    var currency by remember { mutableStateOf(start.currency) }
    var prepay by remember { mutableStateOf(start.prepay) }
    var deposit by remember { mutableStateOf(Money.inputValue(start.deposit)) }
    var paymentUrl by remember { mutableStateOf(start.paymentUrl) }
    var paymentNote by remember { mutableStateOf(start.paymentNote) }
    var payWithin by remember { mutableStateOf(start.payWithinMinutes.toString()) }
    var cancelHours by remember { mutableStateOf(start.cancelHours.toString()) }
    var lateCancel by remember { mutableStateOf(start.lateCancel) }
    var policyText by remember { mutableStateOf(start.policyText) }
    val offers = remember {
        mutableStateListOf<OfferDraft>().apply {
            start.packages.forEach { add(OfferDraft(it.id, it.title, it.sessions.toString(), Money.inputValue(it.price), it.validDays.toString())) }
        }
    }
    var error by remember { mutableStateOf<String?>(null) }
    var proError by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    val number = KeyboardOptions(keyboardType = KeyboardType.Number)
    val decimal = KeyboardOptions(keyboardType = KeyboardType.Decimal)
    val symbol = Money.symbol(currency)

    fun build(): BookingPageFields? {
        val priceMinor = Money.parseInput(price)
        val depositMinor = Money.parseInput(deposit)
        if (priceMinor == null || depositMinor == null) {
            error = "Цена — число, например 2500 или 2500,50"
            return null
        }
        val packages = offers.map { draft ->
            val offerPrice = Money.parseInput(draft.price)
            if (offerPrice == null) {
                error = "Цена пакета «${draft.title.ifBlank { "без названия" }}» — число"
                return null
            }
            PackageOffer(draft.id, draft.title, draft.sessions.toIntOrNull() ?: 0, offerPrice, draft.days.toIntOrNull() ?: 0)
        }
        return BookingPageFields(
            title = title, description = description, durationMinutes = duration, daysAhead = days,
            maxPerDay = perDay.toIntOrNull() ?: 0, noticeHours = notice.toIntOrNull() ?: -1,
            maxSeries = series.toIntOrNull() ?: 0, weekends = weekends,
            guestReminders = reminders, hideBranding = hideBranding,
            price = priceMinor, currency = currency, prepay = prepay, deposit = depositMinor,
            paymentUrl = paymentUrl, paymentNote = paymentNote, payWithinMinutes = payWithin.toIntOrNull() ?: 0,
            cancelHours = cancelHours.toIntOrNull() ?: -1, lateCancel = lateCancel, policyText = policyText,
            packages = packages,
        )
    }

    InsetSheet(onDismiss = onClose, title = if (page == null) "Новая страница записи" else "Страница записи",
        subtitle = page?.title ?: "Ссылка, по которой клиенты записываются сами") {
        Column(
            Modifier.fillMaxWidth().heightIn(max = 640.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Group(head = "Услуга", foot = "Гости видят только свободное время в ваших рабочих часах — без названий событий.") {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    SelfvoyField(title, { title = it; error = null }, "Название, например «Урок математики»", filled = false)
                    SelfvoyField(description, { description = it; error = null }, "Описание для клиентов — необязательно", maxLines = 4)
                    Text("Длительность", style = MaterialTheme.typography.labelMedium, color = Fg)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        listOf(15 to "15 мин", 30 to "30 мин", 45 to "45 мин", 60 to "1 час", 90 to "1,5 часа", 120 to "2 часа").forEach { (value, label) ->
                            SelfvoyChip(label, active = duration == value) { duration = value }
                        }
                    }
                }
            }

            Group(head = "Расписание") {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("Запись открыта на", style = MaterialTheme.typography.labelMedium, color = Fg)
                    Segmented(listOf("7" to "Неделя", "14" to "2 недели", "30" to "Месяц", "60" to "2 месяца"), days.toString()) { days = it.toInt() }
                }
                GroupDivider()
                NumberRow("Встреч в день", "не больше, 1–16", perDay, "", number) { perDay = it.filter(Char::isDigit).take(2); error = null }
                GroupDivider()
                NumberRow("Запас до встречи", "записаться можно не позже, 0–168", notice, "ч", number) { notice = it.filter(Char::isDigit).take(3); error = null }
                GroupDivider()
                NumberRow("Серия", "больше 1 — встречи раз в неделю", series, "", number) { series = it.filter(Char::isDigit).take(2); error = null }
                GroupDivider()
                SwitchRow("Выходные", if (weekends) "Будни и выходные" else "Только будни", weekends) { weekends = it }
            }

            Group(
                head = "Цена и оплата",
                foot = if (prepay == BookingPolicy.PREPAY_NONE) "Цена видна клиенту на странице записи. Без предоплаты — бесплатно."
                    else "Время держится ${payWithin.ifEmpty { "60" }} мин — не оплачено к сроку, запись отменится сама.",
            ) {
                NumberRow("Цена", "0 — не показывать", price, symbol, decimal) { price = it.take(14); error = null }
                GroupDivider()
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Валюта", style = MaterialTheme.typography.labelMedium, color = Fg)
                    Segmented(Money.CURRENCIES.map { it.code to it.symbol }, currency) { currency = it }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Предоплата", style = MaterialTheme.typography.labelMedium, color = Fg)
                        androidx.compose.foundation.layout.Spacer(Modifier.width(6.dp))
                        ProBadge()
                    }
                    Segmented(listOf("none" to "Нет", "full" to "Вся сумма", "deposit" to "Депозит"), prepay) { prepay = it; error = null }
                    if (proLocked && start.prepay == BookingPolicy.PREPAY_NONE && prepay != BookingPolicy.PREPAY_NONE) {
                        Muted("Включить предоплату можно на тарифе Pro.")
                    }
                }
                if (prepay == BookingPolicy.PREPAY_DEPOSIT) {
                    GroupDivider()
                    NumberRow("Депозит", "от 1 до цены", deposit, symbol, decimal) { deposit = it.take(14); error = null }
                }
                if (prepay != BookingPolicy.PREPAY_NONE) {
                    GroupDivider()
                    val yk = payments
                    ListRow("ЮKassa", if (yk?.connected == true) "Магазин ${yk?.shopId.orEmpty()}: карта и СБП" else "Не подключена — Ещё → Подключения") {
                        if (yk?.connected == true) StatusPill(StatusView("Подключена", Tone.GREEN, StatusIcon.CHECK))
                    }
                    GroupDivider()
                    Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        SelfvoyField(paymentUrl, { paymentUrl = it.trim(); error = null }, "Ссылка оплаты: https://…",
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri))
                        SelfvoyField(paymentNote, { paymentNote = it.replace("\n", " "); error = null }, "Реквизиты или СБП: +7 900 000-00-00, Т-Банк")
                    }
                    GroupDivider()
                    NumberRow("Срок оплаты", "15–1440 минут", payWithin, "мин", number) { payWithin = it.filter(Char::isDigit).take(4); error = null }
                }
            }

            Group(
                head = "Отмена и перенос",
                foot = if ((cancelHours.toIntOrNull() ?: 0) == 0) "Клиент может перенести или отменить встречу в любое время."
                    else "Позже чем за $cancelHours ч — ${if (lateCancel == BookingPolicy.LATE_BLOCK) "только через вас" else "без возврата оплаты"}.",
            ) {
                NumberRow("Бесплатная отмена", "не позже чем за N часов; 0 — всегда", cancelHours, "ч", number) {
                    cancelHours = it.filter(Char::isDigit).take(3); error = null
                }
                if ((cancelHours.toIntOrNull() ?: 0) > 0) {
                    GroupDivider()
                    Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("Отмена позже срока", style = MaterialTheme.typography.labelMedium, color = Fg)
                        Segmented(listOf("allow" to "Без возврата", "block" to "Запретить"), lateCancel) { lateCancel = it }
                    }
                }
                GroupDivider()
                Column(Modifier.padding(14.dp)) {
                    SelfvoyField(policyText, { policyText = it; error = null }, "Текст правил для клиента — необязательно", maxLines = 4)
                }
            }

            Group(head = "Клиенту") {
                SwitchRow("Напоминания", "За сутки и за 2 часа — письмом и в Telegram, с кнопкой «Приду»", reminders) { reminders = it }
                GroupDivider()
                SwitchRow(
                    "Скрыть «Запись через Selfvoy»",
                    if (proLocked && !start.hideBranding) "Доступно на тарифе Pro" else "Подпись внизу страницы записи и писем",
                    hideBranding,
                    enabled = !(proLocked && !start.hideBranding) || hideBranding,
                    titleBadge = { ProBadge() },
                ) { hideBranding = it }
            }

            Group(
                head = "Пакеты и абонементы",
                headBadge = { ProBadge() },
                foot = "Клиент покупает пакет при записи и потом записывается по ссылке пакета. До ${PackageOffer.MAX_OFFERS} предложений.",
            ) {
                offers.forEachIndexed { index, draft ->
                    if (index > 0) GroupDivider()
                    Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            SelfvoyField(draft.title, { draft.title = it.replace("\n", " "); error = null }, "Например, «10 занятий»", Modifier.weight(1f))
                            GlassButton("", { offers.remove(draft) }, Modifier.padding(start = 6.dp), icon = Icons.Outlined.Delete, danger = true, height = 40.dp)
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            SelfvoyField(draft.sessions, { draft.sessions = it.filter(Char::isDigit).take(3); error = null }, "10",
                                Modifier.weight(1f), label = "Занятий", keyboardOptions = number)
                            SelfvoyField(draft.price, { draft.price = it.take(14); error = null }, "18000",
                                Modifier.weight(1.3f), label = "Цена, $symbol", keyboardOptions = decimal)
                            SelfvoyField(draft.days, { draft.days = it.filter(Char::isDigit).take(3); error = null }, "90",
                                Modifier.weight(1f), label = "Дней", keyboardOptions = number)
                        }
                    }
                }
                if (offers.size < PackageOffer.MAX_OFFERS) {
                    if (offers.isNotEmpty()) GroupDivider()
                    ListRow("Добавить пакет", if (proLocked && start.packages.isEmpty()) "Доступно на тарифе Pro" else "", Icons.Outlined.Add,
                        onClick = { offers.add(OfferDraft(0, "", "10", "", "90")) })
                }
            }

            if (proError) ProRequiredNotice { onClose(); AppStore.selectTab(AppStore.Tab.MORE) }
            error?.let { NoticeBox(it, Tone.RED) }
            GradientButton(
                if (page == null) "Создать страницу" else "Сохранить",
                {
                    val fields = build()
                    if (fields != null) {
                        error = fields.validate()
                        proError = false
                        if (error == null) {
                            busy = true
                            scope.launch {
                                val result = AppStore.saveBookingPage(page?.id, fields)
                                busy = false
                                if (result.ok) {
                                    AppStore.notify(if (page == null) "Страница создана — поделитесь ссылкой" else "Сохранено")
                                    onClose()
                                } else {
                                    proError = result.proRequired
                                    error = if (result.proRequired) null else result.error
                                }
                            }
                        }
                    }
                },
                Modifier.fillMaxWidth(), busy = busy, enabled = !busy,
            )
        }
    }
}

/** Строка с числом справа (.cell с полем): подпись, пояснение, поле и единица. */
@Composable
private fun NumberRow(label: String, hint: String, value: String, suffix: String, keyboard: KeyboardOptions, onChange: (String) -> Unit) {
    ListRow(label, hint) {
        SelfvoyField(value, onChange, "0", Modifier.width(96.dp), keyboardOptions = keyboard)
        if (suffix.isNotEmpty()) Text(suffix, style = MaterialTheme.typography.bodyMedium, color = Fg)
    }
}

internal fun copyText(context: Context, text: String) {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
    clipboard.setPrimaryClip(ClipData.newPlainText("Selfvoy", text))
}

/** Системное меню «Поделиться» Android (Telegram, VK, сообщения…). */
internal fun shareText(context: Context, title: String, text: String) {
    val intent = Intent(Intent.ACTION_SEND).apply {
        type = "text/plain"
        putExtra(Intent.EXTRA_SUBJECT, title)
        putExtra(Intent.EXTRA_TEXT, text)
    }
    context.startActivity(Intent.createChooser(intent, title).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
}
