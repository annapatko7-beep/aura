package app.selfvoy.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.Remove
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.kit.BookingPage
import app.selfvoy.kit.Money
import app.selfvoy.kit.PackageIssueRequest
import app.selfvoy.kit.PackageIssued
import app.selfvoy.kit.PackagePurchase
import app.selfvoy.kit.Payment
import app.selfvoy.ui.theme.FillSoft
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.InsetSheet
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.NoticeBox
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.Segmented
import app.selfvoy.ui.theme.SelfvoyAvatar
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
 * «Абонементы» — пакеты занятий клиентов (package.*, F5): остаток, срок, оплата.
 * Выдать пакет вне сайта (package.issue), поправить остаток (package.adjust),
 * отменить (package.cancel), отметить оплату (package.paymentSet).
 */
@Composable
fun PackagesScreen() {
    val packages by AppStore.packages.collectAsState()
    val pages by AppStore.bookingPages.collectAsState()
    val pending by AppStore.pendingAction.collectAsState()
    var filter by remember { mutableStateOf("active") }
    var issuing by remember { mutableStateOf(false) }
    var opened by remember { mutableStateOf<Long?>(null) }

    LaunchedEffect(pending) {
        if (pending == AppStore.PendingAction.ISSUE_PACKAGE) {
            issuing = true
            AppStore.consumePendingAction()
        }
    }

    val list = packages.orEmpty().filter { item ->
        when (filter) {
            "active" -> item.status == PackagePurchase.ACTIVE
            "waiting" -> item.status == PackagePurchase.PENDING || item.payment?.awaiting == true
            else -> true
        }
    }

    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item { RoundIconButton(Icons.AutoMirrored.Outlined.ArrowBack, "Назад", { AppStore.selectTab(AppStore.Tab.MORE) }, size = 38.dp) }
        item {
            ScreenHeader("Абонементы") {
                GlassButton("Выдать", { issuing = true }, icon = Icons.Outlined.Add, height = 38.dp)
            }
        }
        item { Muted("Пакеты занятий: клиент записывается по ссылке пакета, занятия списываются сами. Остаток 1 — придёт напоминание продлить.") }
        item { Segmented(listOf("active" to "Активные", "waiting" to "Ждут оплаты", "all" to "Все"), filter) { filter = it } }
        when {
            packages == null -> item { Muted("Загружаем…") }
            list.isEmpty() -> item {
                Muted(when (filter) {
                    "active" -> "Активных абонементов нет. Предложения пакетов настраиваются на странице записи, а выдать пакет можно кнопкой «Выдать»."
                    "waiting" -> "Все пакеты оплачены."
                    else -> "Абонементов пока нет."
                })
            }
            else -> item {
                Group {
                    list.forEachIndexed { index, item ->
                        if (index > 0) GroupDivider()
                        PackageRow(item) { opened = item.id }
                    }
                }
            }
        }
    }

    if (issuing) IssueSheet(pages.orEmpty()) { issuing = false }
    opened?.let { id -> packages.orEmpty().firstOrNull { it.id == id } }?.let { item ->
        PackageSheet(item) { opened = null }
    }
}

fun packageStatusView(item: PackagePurchase): StatusView = when (item.status) {
    PackagePurchase.PENDING -> StatusView("Ждёт оплаты", Tone.AMBER, StatusIcon.CLOCK)
    PackagePurchase.ACTIVE -> if (item.sessionsLeft <= 1) StatusView("Осталось ${item.sessionsLeft}", Tone.NEW, StatusIcon.BELL)
        else StatusView("Активен", Tone.GREEN, StatusIcon.CHECK)
    PackagePurchase.USED -> StatusView("Занятия закончились", Tone.NEUTRAL, StatusIcon.MINUS)
    PackagePurchase.EXPIRED -> StatusView("Срок истёк", Tone.NEUTRAL, StatusIcon.CLOCK)
    else -> StatusView("Отменён", Tone.RED, StatusIcon.X)
}

@Composable
private fun PackageRow(item: PackagePurchase, onOpen: () -> Unit) {
    ListRow(
        title = item.guestName.ifEmpty { "Клиент" },
        subtitle = "${item.title} · ${item.sessionsLeft} из ${item.sessionsTotal}" +
            if (item.validUntil.isNotEmpty()) " · до ${dateLabelOf(item.validUntil)}" else "",
        onClick = onOpen,
    ) {
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(6.dp)) {
            StatusPill(packageStatusView(item))
            SessionsBar(item.sessionsLeft, item.sessionsTotal, Modifier.width(96.dp))
        }
    }
}

/** Полоска остатка занятий (.bar). */
@Composable
private fun SessionsBar(left: Int, total: Int, modifier: Modifier = Modifier) {
    val share = if (total > 0) (left.toFloat() / total).coerceIn(0f, 1f) else 0f
    Box(modifier.height(4.dp).clip(RoundedCornerShape(2.dp)).background(FillSoft)) {
        Box(Modifier.fillMaxHeight().fillMaxWidth(share).clip(RoundedCornerShape(2.dp)).background(Tint))
    }
}

private fun dateLabelOf(iso: String): String = parseInstant(iso)?.toLocalDate()?.let { dateLabel(it) } ?: ""

/** Карточка абонемента: остаток (±), оплата, отмена. */
@Composable
private fun PackageSheet(item: PackagePurchase, onDismiss: () -> Unit) {
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var confirmCancel by remember { mutableStateOf(false) }

    fun run(key: String, action: suspend () -> AppStore.Outcome<*>) {
        busy = key
        error = null
        scope.launch {
            val result = action()
            busy = ""
            if (result.ok) confirmCancel = false else error = result.error
        }
    }

    InsetSheet(onDismiss = onDismiss, title = item.guestName.ifEmpty { "Клиент" }, subtitle = "${item.title} · ${item.pageTitle}",
        leading = { SelfvoyAvatar(item.guestName.ifEmpty { "?" }, size = 52.dp) }) {
        Column(Modifier.fillMaxWidth().heightIn(max = 600.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Group {
                ListRow("Статус") { StatusPill(packageStatusView(item)) }
                GroupDivider()
                ListRow("Осталось занятий", "использовано ${item.sessionsUsed} из ${item.sessionsTotal}") {
                    if (item.editable) {
                        RoundIconButton(Icons.Outlined.Remove, "Убрать занятие", { run("minus") { AppStore.adjustPackage(item, -1) } },
                            size = 32.dp, enabled = busy.isEmpty() && item.sessionsLeft > 0)
                    }
                    Text("${item.sessionsLeft}", style = MaterialTheme.typography.titleLarge, color = Fg, modifier = Modifier.padding(horizontal = 6.dp))
                    if (item.editable) {
                        RoundIconButton(Icons.Outlined.Add, "Добавить занятие", { run("plus") { AppStore.adjustPackage(item, 1) } },
                            size = 32.dp, enabled = busy.isEmpty())
                    }
                }
                GroupDivider()
                ListRow("Действует", if (item.validUntil.isNotEmpty()) "до ${dateLabelOf(item.validUntil)}" else "${item.validDays} дней с момента оплаты")
                if (item.price > 0) {
                    GroupDivider()
                    ListRow("Цена", value = Money.format(item.price, item.currency))
                }
                if (item.guestEmail.isNotEmpty()) {
                    GroupDivider()
                    ListRow("Email", value = item.guestEmail)
                }
                if (item.ccEmail.isNotEmpty()) {
                    GroupDivider()
                    ListRow("Копия извещений", value = item.ccEmail)
                }
            }

            item.payment?.let { payment ->
                Group(head = "Оплата") {
                    ListRow(Money.format(payment.amount, payment.currency), if (payment.method == "yookassa") "ЮKassa" else "по ссылке или реквизитам") {
                        paymentStatusView(payment.status)?.let { StatusPill(it) }
                    }
                }
                payment.ownerActions.forEach { status ->
                    val label = ownerPaymentActionLabel(status)
                    val action = { run("pay-$status") { AppStore.setPackagePayment(item, status) } }
                    if (status == Payment.PAID) {
                        GradientButton(label, action, Modifier.fillMaxWidth(), icon = Icons.Outlined.Check, busy = busy == "pay-$status", enabled = busy.isEmpty())
                    } else if (status == Payment.REFUNDED) {
                        GlassButton(label, action, Modifier.fillMaxWidth(), danger = true, enabled = busy.isEmpty(), height = 50.dp)
                    } else {
                        SoftButton(label, action, Modifier.fillMaxWidth(), busy = busy == "pay-$status", enabled = busy.isEmpty(), height = 50.dp)
                    }
                }
            }

            error?.let { NoticeBox(it, Tone.RED) }
            if (item.editable) {
                if (confirmCancel) {
                    NoticeBox("Отменить абонемент? По ссылке пакета больше не записаться; сделанные записи останутся — их отменяют отдельно." +
                        if (item.payment?.method == "yookassa" && item.payment?.status == Payment.PAID && item.sessionsUsed == 0) " Оплата через ЮKassa вернётся клиенту." else "",
                        Tone.RED) {
                        GlassButton("Да, отменить", { run("cancel") { AppStore.cancelPackage(item) } }, danger = true, height = 36.dp, enabled = busy.isEmpty())
                        GlassButton("Нет", { confirmCancel = false }, quiet = true, height = 36.dp)
                    }
                } else {
                    GlassButton("Отменить абонемент", { confirmCancel = true }, Modifier.fillMaxWidth(), danger = true, height = 46.dp)
                }
            }
        }
    }
}

/** Выдать пакет вне сайта: по предложению страницы или свой. Ссылка пакета — один раз. */
@Composable
private fun IssueSheet(pages: List<BookingPage>, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var pageId by remember { mutableStateOf(pages.firstOrNull { it.active }?.id ?: pages.firstOrNull()?.id ?: 0L) }
    val page = pages.firstOrNull { it.id == pageId }
    var offerId by remember(pageId) { mutableStateOf(page?.packages?.firstOrNull()?.id ?: 0L) }
    var title by remember { mutableStateOf("") }
    var sessions by remember { mutableStateOf("10") }
    var price by remember { mutableStateOf("") }
    var days by remember { mutableStateOf("90") }
    var guestName by remember { mutableStateOf("") }
    var guestEmail by remember { mutableStateOf("") }
    var ccEmail by remember { mutableStateOf("") }
    var paid by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var issued by remember { mutableStateOf<PackageIssued?>(null) }
    val number = KeyboardOptions(keyboardType = KeyboardType.Number)

    InsetSheet(onDismiss = onDismiss, title = "Выдать абонемент", subtitle = "Пакет, проданный не на сайте — клиенту придёт письмо со ссылкой") {
        Column(Modifier.fillMaxWidth().heightIn(max = 620.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            val done = issued
            if (done != null) {
                NoticeBox("Абонемент выдан. Ссылка пакета показывается один раз — отправьте её клиенту.", Tone.GREEN)
                if (done.link.isNotEmpty()) {
                    Group { ListRow(done.link) }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        GradientButton("Поделиться", { shareText(context, done.purchase.title, "Ваш абонемент «${done.purchase.title}»: ${done.link}") },
                            Modifier.weight(1f), icon = Icons.Outlined.Share)
                        GlassButton("Скопировать", { copyText(context, done.link); AppStore.notify("Ссылка скопирована") },
                            Modifier.weight(1f), icon = Icons.Outlined.ContentCopy, height = 50.dp)
                    }
                }
                GlassButton("Готово", onDismiss, Modifier.fillMaxWidth(), quiet = true)
                return@Column
            }
            if (pages.isEmpty()) {
                NoticeBox("Сначала создайте страницу записи — абонемент привязывается к ней.", Tone.AMBER)
                return@Column
            }
            if (pages.size > 1) {
                Group(head = "Страница") {
                    pages.forEachIndexed { index, item ->
                        if (index > 0) GroupDivider()
                        ListRow(item.title, onClick = { pageId = item.id }) {
                            if (item.id == pageId) Icon(Icons.Outlined.Check, contentDescription = "Выбрана", tint = Tint)
                        }
                    }
                }
            }
            Group(head = "Пакет") {
                page?.packages.orEmpty().forEach { offer ->
                    ListRow(offer.title, "${offer.sessions} занятий · ${Money.format(offer.price, page?.currency ?: "RUB")} · ${offer.validDays} дней",
                        onClick = { offerId = offer.id }) {
                        if (offerId == offer.id) Icon(Icons.Outlined.Check, contentDescription = "Выбран", tint = Tint)
                    }
                    GroupDivider()
                }
                ListRow("Свой пакет", "Название, занятия и цена — ниже", onClick = { offerId = 0 }) {
                    if (offerId == 0L) Icon(Icons.Outlined.Check, contentDescription = "Выбран", tint = Tint)
                }
                if (offerId == 0L) {
                    GroupDivider()
                    Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        SelfvoyField(title, { title = it.replace("\n", " "); error = null }, "Например, «8 занятий»", label = "Название")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            SelfvoyField(sessions, { sessions = it.filter(Char::isDigit).take(3) }, "10", Modifier.weight(1f), label = "Занятий", keyboardOptions = number)
                            SelfvoyField(price, { price = it.take(14) }, "0", Modifier.weight(1.3f), label = "Цена, ${Money.symbol(page?.currency ?: "RUB")}",
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal))
                            SelfvoyField(days, { days = it.filter(Char::isDigit).take(3) }, "90", Modifier.weight(1f), label = "Дней", keyboardOptions = number)
                        }
                    }
                }
            }
            Group(head = "Клиент") {
                Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    SelfvoyField(guestName, { guestName = it; error = null }, "Имя клиента")
                    SelfvoyField(guestEmail, { guestEmail = it.trim(); error = null }, "Email — пришлём ссылку пакета",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email))
                    SelfvoyField(ccEmail, { ccEmail = it.trim(); error = null }, "Копия, например родителю — необязательно",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email))
                }
                GroupDivider()
                SwitchRow("Уже оплачено", if (paid) "Оплата отмечена сразу" else "Оплату отметите позже", paid) { paid = it }
            }
            error?.let { NoticeBox(it, Tone.RED) }
            GradientButton("Выдать абонемент", {
                val priceMinor = Money.parseInput(price)
                if (offerId == 0L && priceMinor == null) {
                    error = "Цена — число, например 8000"
                    return@GradientButton
                }
                val request = PackageIssueRequest(
                    pageId = pageId, guestName = guestName, offerId = offerId,
                    title = title, sessions = sessions.toIntOrNull() ?: 0, price = priceMinor ?: 0,
                    validDays = days.toIntOrNull() ?: 0, guestEmail = guestEmail, ccEmail = ccEmail, paid = paid,
                )
                error = request.validate()
                if (error == null) {
                    busy = true
                    scope.launch {
                        val result = AppStore.issuePackage(request)
                        busy = false
                        if (result.ok) issued = result.value else error = result.error
                    }
                }
            }, Modifier.fillMaxWidth(), busy = busy, enabled = !busy)
        }
    }
}
