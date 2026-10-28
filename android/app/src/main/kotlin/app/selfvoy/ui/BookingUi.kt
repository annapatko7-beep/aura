package app.selfvoy.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Block
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.Close
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.Mail
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.kit.Booking
import app.selfvoy.kit.BookingMoney
import app.selfvoy.kit.BookingOccurrence
import app.selfvoy.kit.BookingPage
import app.selfvoy.kit.Money
import app.selfvoy.kit.Payment
import app.selfvoy.kit.SelfvoyErrorCode
import app.selfvoy.kit.SlotsPost
import app.selfvoy.ui.theme.Amber
import app.selfvoy.ui.theme.Card
import app.selfvoy.ui.theme.CapsuleColors
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Green
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.Hairline
import app.selfvoy.ui.theme.InsetSheet
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.NoticeBox
import app.selfvoy.ui.theme.Red
import app.selfvoy.ui.theme.Sec
import app.selfvoy.ui.theme.Segmented
import app.selfvoy.ui.theme.SelfvoyAvatar
import app.selfvoy.ui.theme.SoftButton
import app.selfvoy.ui.theme.StatusIcon
import app.selfvoy.ui.theme.StatusPill
import app.selfvoy.ui.theme.StatusView
import app.selfvoy.ui.theme.Tint
import app.selfvoy.ui.theme.Tone
import app.selfvoy.ui.theme.statusIconVector
import kotlinx.coroutines.launch
import java.time.Duration
import java.time.LocalDate
import java.time.LocalDateTime

/*
  Записи владельца в языке 2026 — общая логика экранов «Сегодня» и «Запись»
  (зеркало web/src/lib/today.ts, payment.ts и components/BookingSheet.tsx):
  статус встречи — значок и слово, лента дня, карточка записи с оплатой.
*/

// ------------------------------------------------------------------ статусы

private fun ldt(iso: String): LocalDateTime? = parseInstant(iso)

/** Подпись статуса оплаты: владельцу — что сделать. */
fun paymentStatusView(status: String): StatusView? = when (status) {
    Payment.PENDING -> StatusView("Ждём оплату", Tone.AMBER, StatusIcon.CLOCK)
    Payment.CLAIMED -> StatusView("Проверьте оплату", Tone.AMBER, StatusIcon.HAND)
    Payment.PAID -> StatusView("Оплачено", Tone.GREEN, StatusIcon.CHECK)
    Payment.REFUNDED -> StatusView("Возврат", Tone.NEUTRAL, StatusIcon.UNDO)
    Payment.WAIVED -> StatusView("Без оплаты", Tone.NEUTRAL, StatusIcon.MINUS)
    Payment.EXPIRED -> StatusView("Не оплачено", Tone.RED, StatusIcon.X)
    else -> null
}

/** Подпись кнопки владельца для статуса оплаты. */
fun ownerPaymentActionLabel(status: String): String = when (status) {
    Payment.PAID -> "Отметить оплату"
    Payment.WAIVED -> "Без оплаты"
    else -> "Возврат"
}

/**
 * Статус встречи для ленты: важнее всего то, что требует действия — оплата; затем
 * «Приду»; новая запись (за сутки) — «внимание».
 */
fun occurrenceStatus(booking: Booking, occurrence: BookingOccurrence, now: LocalDateTime = LocalDateTime.now()): StatusView {
    if (!booking.active || !occurrence.active) return StatusView("Отменена", Tone.RED, StatusIcon.X)
    val payment = booking.payment
    if (payment != null && payment.awaiting) return paymentStatusView(payment.status)!!
    if (occurrence.confirmed) return StatusView("Придёт", Tone.GREEN, StatusIcon.CHECK)
    val end = ldt(occurrence.end) ?: ldt(occurrence.start)
    if (end != null && !end.isAfter(now)) return StatusView("Прошла", Tone.NEUTRAL, StatusIcon.CHECK)
    val created = ldt(booking.createdAt)
    if (created != null && Duration.between(created, now).toHours() < 24) return StatusView("Новая запись", Tone.NEW, StatusIcon.PLUS)
    if (payment?.status == Payment.PAID) return StatusView("Оплачено", Tone.GREEN, StatusIcon.CHECK)
    return StatusView("Записан(а)", Tone.NEUTRAL, StatusIcon.CALENDAR)
}

/** Номер цвета капсулы страницы: по порядку создания страниц, 5 цветов по кругу. */
fun pageColor(pageId: Long, pages: List<BookingPage>?): Color {
    val ids = pages.orEmpty().map { it.id }.distinct().sorted()
    val index = ids.indexOf(pageId).let { if (it >= 0) it else (kotlin.math.abs(pageId) % CapsuleColors.size).toInt() }
    return CapsuleColors[index % CapsuleColors.size]
}

/** «сегодня, 10:00» / «завтра, 10:00» / «пт, 23 октября, 10:00». */
fun whenText(iso: String, now: LocalDate = LocalDate.now()): String {
    val at = ldt(iso) ?: return ""
    val day = when (at.toLocalDate()) {
        now -> "сегодня"
        now.plusDays(1) -> "завтра"
        now.minusDays(1) -> "вчера"
        else -> "${dayShort(at.toLocalDate()).lowercase()}, ${dateLabel(at.toLocalDate())}"
    }
    return "$day, ${timeOf(iso)}"
}

/** «через 1 ч 13 мин», «идёт сейчас», «завтра в 10:00», «пт, 23 октября». */
fun untilLabel(start: LocalDateTime, end: LocalDateTime, now: LocalDateTime = LocalDateTime.now()): String {
    val minutes = Duration.between(now, start).toMinutes()
    if (minutes <= 0) return if (end.isAfter(now)) "идёт сейчас" else "началась"
    val today = now.toLocalDate()
    return when (start.toLocalDate()) {
        today -> if (minutes < 60) "через $minutes мин" else "через ${minutes / 60} ч" + if (minutes % 60 != 0L) " ${minutes % 60} мин" else ""
        today.plusDays(1) -> "завтра в ${"%02d:%02d".format(start.hour, start.minute)}"
        else -> "${dayShort(start.toLocalDate()).lowercase()}, ${dateLabel(start.toLocalDate())}"
    }
}

/** Ближайшая встреча: идущая сейчас или следующая (активная запись и встреча). */
data class NextUp(val booking: Booking, val occurrence: BookingOccurrence, val start: LocalDateTime, val end: LocalDateTime)

fun nextUp(bookings: List<Booking>, now: LocalDateTime = LocalDateTime.now()): NextUp? {
    var best: NextUp? = null
    for (booking in bookings) {
        if (!booking.active) continue
        for (occurrence in booking.occurrences) {
            if (!occurrence.active) continue
            val start = ldt(occurrence.start) ?: continue
            val end = ldt(occurrence.end) ?: start
            if (!end.isAfter(now)) continue
            if (best == null || start.isBefore(best.start)) best = NextUp(booking, occurrence, start, end)
        }
    }
    return best
}

// ------------------------------------------------------------------ лента дня

sealed class DayEntry {
    abstract val start: LocalDateTime
    abstract val key: String

    data class Meeting(val booking: Booking, val occurrence: BookingOccurrence, override val start: LocalDateTime,
                       val end: LocalDateTime, val color: Color, val status: StatusView) : DayEntry() {
        override val key get() = "b${booking.id}-${occurrence.start}"
    }
    data class TaskItem(val task: AppStore.Task, override val start: LocalDateTime) : DayEntry() {
        override val key get() = "t${task.id}"
    }
    data class Free(val slot: AppStore.FreeSlot, override val start: LocalDateTime, val end: LocalDateTime) : DayEntry() {
        override val key get() = "f${slot.pageId}-${slot.start}"
    }
    data class Now(override val start: LocalDateTime) : DayEntry() {
        override val key get() = "now"
    }
}

/** Сколько свободных окон показывать в ленте дня. */
const val FREE_IN_DAY = 3

/**
 * Лента дня [day]: встречи, задачи со сроком (кроме задач самих записей),
 * свободные окна (будущие, не пересекаются со встречами, не больше [FREE_IN_DAY])
 * и линия «сейчас», если день — сегодня.
 */
fun buildDay(
    bookings: List<Booking>,
    tasks: List<AppStore.Task>,
    free: List<AppStore.FreeSlot>,
    pages: List<BookingPage>?,
    day: LocalDate,
    now: LocalDateTime = LocalDateTime.now(),
): List<DayEntry> {
    val entries = mutableListOf<DayEntry>()
    val busy = mutableListOf<Pair<LocalDateTime, LocalDateTime>>()
    val bookingMinutes = HashSet<LocalDateTime>()
    for (booking in bookings) {
        for (occurrence in booking.occurrences) {
            val start = ldt(occurrence.start) ?: continue
            bookingMinutes += start.withSecond(0).withNano(0)
            if (start.toLocalDate() != day || !booking.active || !occurrence.active) continue
            val end = ldt(occurrence.end) ?: start
            busy += start to end
            entries += DayEntry.Meeting(booking, occurrence, start, end, pageColor(booking.pageId, pages),
                occurrenceStatus(booking, occurrence, now))
        }
    }
    for (task in tasks) {
        if (task.status != "pending") continue
        val at = ldt(task.whenAt) ?: continue
        if (at.toLocalDate() != day || at.withSecond(0).withNano(0) in bookingMinutes) continue
        entries += DayEntry.TaskItem(task, at)
    }
    var shown = 0
    val seen = HashSet<LocalDateTime>()
    for (slot in free.sortedBy { it.start }) {
        if (shown >= FREE_IN_DAY) break
        val start = ldt(slot.start) ?: continue
        val end = ldt(slot.end) ?: continue
        if (start.toLocalDate() != day || !start.isAfter(now) || !seen.add(start)) continue
        if (busy.any { (s, e) -> start.isBefore(e) && end.isAfter(s) }) continue
        busy += start to end
        entries += DayEntry.Free(slot, start, end)
        shown++
    }
    entries.sortWith(compareBy<DayEntry> { it.start }.thenBy { if (it is DayEntry.Free) 1 else 0 })
    if (day == now.toLocalDate() && entries.isNotEmpty()) {
        val index = entries.indexOfFirst { it.start.isAfter(now) }
        if (index == -1) entries += DayEntry.Now(now) else entries.add(index, DayEntry.Now(now))
    }
    return entries
}

/**
 * «3 встречи · 6 000 ₽» — встречи дня и их стоимость: цены записей (старый сервер —
 * цены страниц), без занятий по абонементу; одна валюта.
 */
fun daySummary(entries: List<DayEntry>, pages: List<BookingPage>?): String {
    val meetings = entries.filterIsInstance<DayEntry.Meeting>()
    val (amount, currency) = BookingMoney.dayTotal(meetings.map { it.booking }, pages)
    val count = meetings.size.toLong()
    val text = if (count > 0) "$count ${plural(count, "встреча", "встречи", "встреч")}" else "Встреч нет"
    return if (amount > 0) "$text · ${Money.format(amount, currency)}" else text
}

/** Строка оплаты под ценой: «Депозит 1 000 ₽ · ждём оплату» (статус — ещё и значком рядом). */
fun paymentLine(payment: Payment): String {
    val view = paymentStatusView(payment.status) ?: return BookingMoney.paymentText(payment)
    return "${BookingMoney.paymentText(payment)} · ${view.label.replaceFirstChar { it.lowercaseChar() }}"
}

// ------------------------------------------------------------- история записи

data class HistoryStep(val key: String, val title: String, val detail: String, val tone: Tone, val icon: StatusIcon)

/** Шаги истории: запись → оплата → напоминания → «Приду» → отмена (для встречи [start] или ближайшей). */
fun bookingHistory(booking: Booking, start: String?, now: LocalDateTime = LocalDateTime.now()): List<HistoryStep> {
    val steps = mutableListOf<HistoryStep>()
    steps += HistoryStep("created", "Записался(ась) по ссылке", whenText(booking.createdAt), Tone.GREEN, StatusIcon.CHECK)
    booking.payment?.let { payment ->
        val amount = Money.format(payment.amount, payment.currency)
        val what = if (payment.kind == "deposit") "депозит" else "оплату"
        val via = if (payment.method == "yookassa") " · ЮKassa" else " · по ссылке"
        steps += when (payment.status) {
            Payment.PENDING -> HistoryStep("payment", "Ждём $what $amount",
                if (payment.dueAt.isNotEmpty()) "до ${whenText(payment.dueAt)}$via" else "", Tone.AMBER, StatusIcon.CLOCK)
            Payment.CLAIMED -> HistoryStep("payment", "Гость отметил: оплатил(а) $amount",
                "Проверьте поступление и отметьте оплату", Tone.AMBER, StatusIcon.HAND)
            Payment.PAID -> HistoryStep("payment", "Оплачено $amount", whenText(payment.paidAt), Tone.GREEN, StatusIcon.CHECK)
            Payment.WAIVED -> HistoryStep("payment", "Без оплаты", "Вы отметили, что оплата не нужна", Tone.NEUTRAL, StatusIcon.MINUS)
            Payment.REFUNDED -> HistoryStep("payment", "Возврат $amount", whenText(payment.refundedAt), Tone.NEUTRAL, StatusIcon.UNDO)
            else -> HistoryStep("payment", "Оплата не поступила к сроку",
                if (payment.dueAt.isNotEmpty()) "срок был ${whenText(payment.dueAt)}" else "", Tone.RED, StatusIcon.X)
        }
    }
    val focus = focusOccurrence(booking, start, now)
    if (focus != null) {
        val live = booking.active && focus.active
        booking.reminders.filter { it.start == focus.start }.sortedBy { it.dueAt }.forEach { reminder ->
            val what = if (reminder.kind == "24h") "за сутки" else "за 2 часа"
            if (reminder.sent) {
                steps += HistoryStep("rem-${reminder.kind}", "Напоминание $what отправлено", whenText(reminder.sentAt), Tone.GREEN, StatusIcon.CHECK)
            } else if (live) {
                steps += HistoryStep("rem-${reminder.kind}", "Напоминание $what", "запланировано на ${whenText(reminder.dueAt)}", Tone.NEUTRAL, StatusIcon.BELL)
            }
        }
        val upcoming = ldt(focus.start)?.isAfter(now) == true
        if (focus.confirmed) {
            steps += HistoryStep("confirmed", "Подтвердил(а): «Приду»", whenText(focus.confirmedAt), Tone.GREEN, StatusIcon.CHECK)
        } else if (live && upcoming) {
            steps += HistoryStep("confirmed", "Ещё не подтвердил(а)", "Кнопка «Приду» — в напоминании", Tone.NEUTRAL, StatusIcon.BELL)
        }
        if (!focus.active && booking.active) steps += HistoryStep("cancelled-one", "Эта встреча отменена", "", Tone.RED, StatusIcon.X)
    }
    if (!booking.active) {
        val title = when (booking.cancelledBy) {
            "owner" -> "Вы отменили запись"
            "system" -> "Запись отменена: оплата не поступила"
            else -> "Гость отменил запись"
        }
        steps += HistoryStep("cancelled", title, whenText(booking.updatedAt), Tone.RED, StatusIcon.X)
    }
    return steps
}

private fun focusOccurrence(booking: Booking, start: String?, now: LocalDateTime): BookingOccurrence? {
    val list = booking.occurrences
    if (start != null) list.firstOrNull { it.start == start }?.let { return it }
    return list.firstOrNull { it.active && (ldt(it.end.ifEmpty { it.start })?.isAfter(now) == true) } ?: list.lastOrNull()
}

private fun isUpcoming(occurrence: BookingOccurrence, now: LocalDateTime = LocalDateTime.now()): Boolean =
    occurrence.active && (ldt(occurrence.start)?.isAfter(now) == true)

// ------------------------------------------------------------- карточка записи

/**
 * Карточка записи — окно снизу: кто, когда, сколько стоит, статус оплаты, история,
 * действия «Написать / Отменить», отметки оплаты (booking.paymentSet).
 */
@Composable
fun BookingSheet(booking: Booking, start: String?, pages: List<BookingPage>?, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var confirming by remember { mutableStateOf<String?>(null) }

    val list = booking.occurrences
    val focus = (start?.let { s -> list.firstOrNull { it.start == s } }) ?: list.firstOrNull { isUpcoming(it) } ?: list.lastOrNull()
    val page = pages.orEmpty().firstOrNull { it.id == booking.pageId }
    val minutes = focus?.let { o ->
        val a = ldt(o.start)
        val b = ldt(o.end)
        if (a != null && b != null) Duration.between(a, b).toMinutes() else 0L
    } ?: 0L
    val upcoming = list.filter { isUpcoming(it) }
    val payment = booking.payment
    val price = BookingMoney.priceText(booking, page)

    fun run(key: String, action: suspend () -> AppStore.Outcome<*>) {
        busy = key
        error = null
        scope.launch {
            val result = action()
            busy = ""
            if (result.ok) confirming = null else error = result.error
        }
    }

    InsetSheet(
        onDismiss = onDismiss,
        title = booking.guestName.ifEmpty { "Гость" },
        subtitle = listOf(booking.pageTitle.ifEmpty { "Запись" }, if (minutes > 0) "$minutes мин" else "").filter { it.isNotEmpty() }.joinToString(" · "),
        leading = { SelfvoyAvatar(booking.guestName.ifEmpty { "?" }, size = 52.dp) },
    ) {
        Column(
            Modifier.fillMaxWidth().heightIn(max = 620.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            if (focus != null) {
                Group {
                    val at = ldt(focus.start)
                    val range = listOf(timeOf(focus.start), timeOf(focus.end)).filter { it.isNotEmpty() }.joinToString("–")
                    val series = if (list.size > 1) " · встреча ${list.indexOf(focus) + 1} из ${list.size}" else ""
                    ListRow(
                        title = at?.let { "${dayTitle(focus.start)}, ${dateLabel(it.toLocalDate())}" } ?: "Встреча",
                        subtitle = range + series,
                        value = price,
                    )
                    // Оплата онлайн — отдельно от цены: «Депозит 1 000 ₽» + статус (значок и слово).
                    if (payment != null) {
                        GroupDivider()
                        ListRow(BookingMoney.paymentText(payment)) {
                            paymentStatusView(payment.status)?.let { StatusPill(it) }
                        }
                    }
                    GroupDivider()
                    ListRow("Статус") {
                        StatusPill(occurrenceStatus(booking, focus))
                    }
                }
            }

            // Действия: написать, отменить эту / все.
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SheetAction("Написать", Icons.Outlined.Mail, Tint, enabled = booking.guestEmail.isNotEmpty(), modifier = Modifier.weight(1f)) {
                    val intent = Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:" + Uri.encode(booking.guestEmail)))
                    runCatching { context.startActivity(intent) }
                }
                if (focus != null && isUpcoming(focus) && booking.active) {
                    SheetAction(if (upcoming.size > 1) "Отменить эту" else "Отменить", Icons.Outlined.Close, Red, modifier = Modifier.weight(1f)) {
                        confirming = "one"
                    }
                }
                if (upcoming.size > 1 && booking.active) {
                    SheetAction("Отменить все", Icons.Outlined.Block, Red, modifier = Modifier.weight(1f)) { confirming = "all" }
                }
            }

            val confirm = confirming
            if (confirm != null && focus != null) {
                val text = (if (confirm == "one") "Отменить встречу ${whenText(focus.start)}? Гость получит письмо."
                    else "Отменить все предстоящие встречи (${upcoming.size})? Гость получит письмо.") +
                    if (payment?.status == Payment.PAID && confirm == "all") " Оплата вернётся гостю." else ""
                NoticeBox(text, Tone.RED) {
                    GradientButton("Да, отменить", {
                        run("cancel") { AppStore.cancelBooking(booking, if (confirm == "one") focus.start else null) }
                    }, height = 38.dp, busy = busy == "cancel", enabled = busy.isEmpty())
                    GlassButton("Нет", { confirming = null }, quiet = true, height = 38.dp)
                }
            }
            error?.let { NoticeBox(it, Tone.RED) }

            // История записи.
            Group {
                bookingHistory(booking, focus?.start).forEach { step -> HistoryRow(step) }
            }

            if (list.size > 1) {
                Group(head = "Встречи серии") {
                    list.forEachIndexed { index, o ->
                        if (index > 0) GroupDivider()
                        ListRow(whenText(o.start)) {
                            when {
                                !o.active -> StatusPill(StatusView("Отменена", Tone.RED, StatusIcon.X))
                                o.confirmed -> StatusPill(StatusView("Придёт", Tone.GREEN, StatusIcon.CHECK))
                                else -> Unit
                            }
                        }
                    }
                }
            }

            if (booking.guestEmail.isNotEmpty() || booking.ccEmail.isNotEmpty() || booking.guestComment.isNotEmpty() || booking.packageId > 0) {
                Group {
                    var first = true
                    if (booking.guestEmail.isNotEmpty()) {
                        ListRow("Email", value = booking.guestEmail)
                        first = false
                    }
                    if (booking.ccEmail.isNotEmpty()) {
                        if (!first) GroupDivider()
                        ListRow("Копия извещений", value = booking.ccEmail)
                        first = false
                    }
                    if (booking.packageId > 0) {
                        if (!first) GroupDivider()
                        ListRow("По абонементу", "Пакет №${booking.packageId}", onClick = {
                            onDismiss()
                            AppStore.selectTab(AppStore.Tab.PACKAGES)
                        })
                        first = false
                    }
                    if (booking.guestComment.isNotEmpty()) {
                        if (!first) GroupDivider()
                        ListRow("Комментарий", booking.guestComment)
                    }
                }
            }

            val actions = payment?.ownerActions.orEmpty()
            if (payment != null && actions.isNotEmpty()) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    actions.forEachIndexed { index, status ->
                        val label = ownerPaymentActionLabel(status)
                        val action = { run("pay-$status") { AppStore.setBookingPayment(booking, status) } }
                        when {
                            index == 0 && status == Payment.PAID -> GradientButton(label, action, Modifier.fillMaxWidth(),
                                icon = Icons.Outlined.Check, busy = busy == "pay-$status", enabled = busy.isEmpty())
                            status == Payment.REFUNDED -> GlassButton(label, action, Modifier.fillMaxWidth(), danger = true,
                                enabled = busy.isEmpty(), height = 50.dp)
                            else -> SoftButton(label, action, Modifier.fillMaxWidth(), busy = busy == "pay-$status",
                                enabled = busy.isEmpty(), height = 50.dp)
                        }
                    }
                    if (payment.method == "yookassa" && Payment.REFUNDED in actions) {
                        Muted("Отметка «Возврат» не переводит деньги: вернуть оплату через ЮKassa можно, отменив запись целиком.")
                    }
                }
            }
        }
    }
}

@Composable
private fun SheetAction(label: String, icon: ImageVector, color: Color, modifier: Modifier = Modifier, enabled: Boolean = true, onClick: () -> Unit) {
    Column(
        modifier
            .alpha(if (enabled) 1f else 0.45f)
            .clip(RoundedCornerShape(18.dp))
            .background(Card)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(top = 10.dp, bottom = 8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Icon(icon, contentDescription = null, tint = color, modifier = Modifier.size(22.dp))
        Text(label, style = MaterialTheme.typography.labelMedium, color = color, maxLines = 1)
    }
}

@Composable
private fun HistoryRow(step: HistoryStep) {
    val color = when (step.tone) {
        Tone.GREEN -> Green
        Tone.AMBER -> Amber
        Tone.RED -> Red
        else -> Sec
    }
    Row(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 9.dp), verticalAlignment = Alignment.Top) {
        Box(Modifier.size(22.dp).clip(CircleShape).background(color), contentAlignment = Alignment.Center) {
            Icon(statusIconVector(step.icon), contentDescription = null, tint = Color.White, modifier = Modifier.size(13.dp))
        }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(step.title, style = MaterialTheme.typography.titleSmall, color = Fg)
            if (step.detail.isNotEmpty()) Text(step.detail, style = MaterialTheme.typography.bodySmall, color = Sec)
        }
    }
}

// ------------------------------------------------------- пост со свободными окнами

/**
 * «Пост со свободными окнами» (booking.slotsPost): формат и период, готовый текст,
 * «Поделиться» через системное меню Android и «Скопировать».
 */
@Composable
fun SlotsPostSheet(pages: List<BookingPage>, initialPageId: Long?, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val open = pages.filter { it.active }
    var pageId by remember { mutableStateOf(initialPageId?.takeIf { id -> open.any { it.id == id } } ?: open.firstOrNull()?.id) }
    var format by remember { mutableStateOf(SlotsPost.FORMAT_TELEGRAM) }
    var days by remember { mutableStateOf(7) }
    var post by remember { mutableStateOf<SlotsPost?>(null) }
    var error by remember { mutableStateOf<String?>(null) }
    var loading by remember { mutableStateOf(false) }
    var reload by remember { mutableStateOf(0) }
    val page = open.firstOrNull { it.id == pageId }

    LaunchedEffect(pageId, format, days, reload) {
        val id = pageId ?: return@LaunchedEffect
        loading = true
        error = null
        val result = AppStore.slotsPost(id, days, format)
        loading = false
        post = result.value
        error = result.error
    }

    // Без SELFVOY_PUBLIC_URL сервер не добавляет ссылку — дописываем путь сами.
    val text = post?.let { p ->
        if (p.url.isEmpty() && page != null && !p.text.contains("#/b/")) p.text.trimEnd() + "\n\nЗаписаться: " + page.link else p.text
    }.orEmpty()

    InsetSheet(onDismiss = onDismiss, title = "Пост со свободными окнами", subtitle = page?.title.orEmpty()) {
        Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (open.isEmpty()) {
                NoticeBox(if (pages.isEmpty()) "Сначала создайте страницу записи." else "Все страницы на паузе — откройте одну, чтобы сделать пост.", Tone.AMBER)
                return@Column
            }
            if (open.size > 1) {
                Group(head = "Страница") {
                    open.forEachIndexed { index, item ->
                        if (index > 0) GroupDivider()
                        ListRow(item.title, onClick = { pageId = item.id }) {
                            if (item.id == pageId) Icon(Icons.Outlined.Check, contentDescription = "Выбрана", tint = Tint)
                        }
                    }
                }
            }
            Segmented(listOf("telegram" to "Telegram", "vk" to "VK", "plain" to "Текст"), format) { format = it }
            Segmented(listOf("3" to "3 дня", "7" to "Неделя", "14" to "2 недели"), days.toString()) { days = it.toInt() }
            Box(
                Modifier.fillMaxWidth().heightIn(min = 140.dp).clip(RoundedCornerShape(16.dp)).background(Card).padding(14.dp),
            ) {
                when {
                    loading -> CircularProgressIndicator(Modifier.size(22.dp).align(Alignment.Center), color = Tint, strokeWidth = 2.dp)
                    text.isNotEmpty() -> Text(text, style = MaterialTheme.typography.bodyMedium, color = Fg)
                    else -> Muted("Текст появится здесь")
                }
            }
            post?.let { Muted(if (it.slots > 0) "Окон в посте: ${it.slots}" else "Свободных окон в этот период нет — пост скажет об этом честно.") }
            error?.let { NoticeBox(it, Tone.RED) }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GradientButton("Поделиться", { shareText(context, page?.title ?: "Свободные окна", text) }, Modifier.weight(1f),
                    icon = Icons.Outlined.Share, enabled = text.isNotEmpty() && !loading)
                GlassButton("Скопировать", {
                    copyText(context, text)
                    AppStore.notify("Текст скопирован")
                }, Modifier.weight(1f), icon = Icons.Outlined.ContentCopy, enabled = text.isNotEmpty(), height = 50.dp)
            }
            GlassButton("Обновить", { reload++ }, Modifier.fillMaxWidth(), icon = Icons.Outlined.Refresh, quiet = true, enabled = !loading)
        }
    }
}

/** «Нужен Pro» — что делать: пригласить коллегу (оба получают месяц Pro). */
@Composable
fun ProRequiredNotice(onInvite: () -> Unit) {
    NoticeBox(SelfvoyErrorCode.PRO_REQUIRED_TEXT, Tone.AMBER) {
        GradientButton("Пригласить коллегу", onInvite, height = 36.dp)
    }
}

/** Тонкая линия-разделитель внутри карточек. */
@Composable
fun ThinDivider(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().padding(vertical = 2.dp).heightIn(min = 1.dp, max = 1.dp).background(Hairline))
}

/** Открыть ссылку во внешнем приложении (Telegram, браузер, календарь). */
fun openUrl(context: Context, url: String): Boolean = runCatching {
    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
}.isSuccess
