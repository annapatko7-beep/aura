package app.selfvoy.ui

import android.content.Intent
import android.net.Uri
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
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.MenuBook
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.NotificationsNone
import androidx.compose.material.icons.outlined.Star
import androidx.compose.material.icons.outlined.WarningAmber
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.selfvoy.AppStore
import app.selfvoy.kit.BookingMoney
import app.selfvoy.ui.theme.Amber
import app.selfvoy.ui.theme.Bg
import app.selfvoy.ui.theme.BrandBlue
import app.selfvoy.ui.theme.Card
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.FillSoft
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.ProBadge
import app.selfvoy.ui.theme.Red
import app.selfvoy.ui.theme.Sec
import app.selfvoy.ui.theme.SectionTitle
import app.selfvoy.ui.theme.Segmented
import app.selfvoy.ui.theme.SelfvoyAvatar
import app.selfvoy.ui.theme.SoftButton
import app.selfvoy.ui.theme.StatusMark
import app.selfvoy.ui.theme.Ter
import app.selfvoy.ui.theme.Tint
import app.selfvoy.ui.theme.TintSoft
import app.selfvoy.ui.theme.glass
import app.selfvoy.ui.theme.statusIconVector
import kotlinx.coroutines.delay
import java.time.LocalDateTime
import java.time.format.TextStyle
import java.util.Locale

/**
 * «Сегодня» — главный экран владельца (brand/selfvoy-app.html, web Home.tsx):
 * карточка следующей встречи с кнопками и лента дня из настоящих данных —
 * встречи записей (booking.list), задачи со сроком (tasks.list), свободные окна
 * своей страницы (booking.get) с «Предложить» и красная линия «сейчас».
 */
@Composable
fun HomeScreen() {
    val context = LocalContext.current
    val auth by AppStore.auth.collectAsState()
    val tasks by AppStore.tasks.collectAsState()
    val unread by AppStore.unread.collectAsState()
    val confirmations by AppStore.confirmations.collectAsState()
    val pages by AppStore.bookingPages.collectAsState()
    val bookings by AppStore.bookings.collectAsState()
    val free by AppStore.freeSlots.collectAsState()
    val plan by AppStore.plan.collectAsState()
    val me = auth as? AppStore.AuthState.LoggedIn

    // Минутная стрелка: «через N мин» и линия «сейчас» живые.
    var now by remember { mutableStateOf(LocalDateTime.now()) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(30_000)
            now = LocalDateTime.now()
        }
    }
    var dayOffset by remember { mutableStateOf("0") }
    var opened by remember { mutableStateOf<Pair<Long, String?>?>(null) }
    var posting by remember { mutableStateOf(false) }

    val day = now.toLocalDate().plusDays(dayOffset.toLong())
    val entries = buildDay(bookings, tasks, free, pages, day, now)
    val next = nextUp(bookings, now)
    val ru = Locale("ru")
    val eyebrow = "${now.dayOfWeek.getDisplayName(TextStyle.FULL, ru)}, ${dateLabel(now.toLocalDate())}".uppercase(ru)

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        // Шапка: дата, входящие, профиль (Pro — значком).
        Row(Modifier.fillMaxWidth().height(40.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(eyebrow, style = MaterialTheme.typography.labelMedium, color = Sec, modifier = Modifier.weight(1f),
                maxLines = 1, overflow = TextOverflow.Ellipsis)
            val attention = unread + confirmations.size
            Box(
                Modifier.size(38.dp).glass(CircleShape).clickable {
                    AppStore.selectTab(if (confirmations.isNotEmpty()) AppStore.Tab.CONFIRMATIONS else AppStore.Tab.NOTICES)
                },
                contentAlignment = Alignment.Center,
            ) {
                Icon(Icons.Outlined.NotificationsNone, contentDescription = if (attention > 0) "Входящие: $attention" else "Входящие",
                    tint = Fg, modifier = Modifier.size(19.dp))
                if (attention > 0) Box(Modifier.align(Alignment.TopEnd).padding(7.dp).size(9.dp).clip(CircleShape).background(Red))
            }
            Spacer(Modifier.width(8.dp))
            Box(Modifier.clickable { AppStore.selectTab(AppStore.Tab.SETTINGS) }) {
                SelfvoyAvatar(me?.displayName.orEmpty(), size = 38.dp)
                if (plan?.isPro() == true) Box(Modifier.align(Alignment.BottomCenter).offset(y = 6.dp)) { ProBadge() }
            }
        }
        Text("Сегодня", style = MaterialTheme.typography.displayMedium, color = Fg, modifier = Modifier.padding(top = 2.dp, bottom = 14.dp))

        if (confirmations.isNotEmpty()) {
            Group {
                ListRow("Ждут вашего решения: ${confirmations.size}", confirmations.first().summary, Icons.Outlined.WarningAmber, Amber,
                    onClick = { AppStore.selectTab(AppStore.Tab.CONFIRMATIONS) })
            }
            Spacer(Modifier.height(14.dp))
        }

        if (next != null) {
            val page = pages.orEmpty().firstOrNull { it.id == next.booking.pageId }
            val price = BookingMoney.priceText(next.booking, page)
            NextCard(next, now, pageColor(next.booking.pageId, pages), price,
                onOpen = { opened = next.booking.id to next.occurrence.start },
                onWrite = {
                    if (next.booking.guestEmail.isNotEmpty()) {
                        runCatching { context.startActivity(Intent(Intent.ACTION_SENDTO, Uri.parse("mailto:" + Uri.encode(next.booking.guestEmail)))) }
                    } else {
                        AppStore.selectTab(AppStore.Tab.BOOKING)
                    }
                },
            )
        } else {
            EmptyNext(loading = pages == null, noPages = pages?.isEmpty() == true) {
                if (pages?.isEmpty() == true) AppStore.startAction(AppStore.PendingAction.NEW_PAGE) else posting = true
            }
        }

        SectionTitle("День", Modifier.padding(top = 16.dp, bottom = 8.dp), trailing = daySummary(entries, pages))
        Segmented(listOf("0" to "Сегодня", "1" to "Завтра"), dayOffset, Modifier.padding(bottom = 10.dp)) { dayOffset = it }

        if (entries.isEmpty()) {
            Group {
                ListRow(if (dayOffset == "0") "На сегодня ни встреч, ни задач со сроком" else "На завтра пока ничего нет",
                    if (free.isEmpty() && pages?.any { it.active } != true) "Заведите страницу записи — клиенты будут записываться сами" else "")
            }
        } else {
            Timeline(entries, now,
                onOpen = { booking, start -> opened = booking.id to start },
                onTask = { task -> AppStore.setTaskStatus(task.id, "complete") },
                onOffer = { entry ->
                    val target = pages.orEmpty().firstOrNull { it.id == entry.slot.pageId } ?: return@Timeline
                    val text = "Есть свободное окно ${dayPhrase(entry.slot.start)} в ${timeOf(entry.slot.start)} — ${target.title}. Записаться: ${target.link}"
                    shareText(context, target.title, text)
                },
            )
        }

        // Задачи без срока на сегодня — коротким списком.
        val pending = tasks.filter { it.status == "pending" && parseInstant(it.whenAt)?.toLocalDate() != day }
            .sortedBy { it.whenAt.ifEmpty { "9999" } }.take(4)
        if (pending.isNotEmpty()) {
            SectionTitle("Задачи", Modifier.padding(top = 18.dp, bottom = 8.dp))
            Group {
                pending.forEachIndexed { index, task ->
                    if (index > 0) GroupDivider()
                    ListRow(task.title, if (task.whenAt.isNotEmpty()) whenText(task.whenAt) else "", onClick = { AppStore.selectTab(AppStore.Tab.TASKS) }) {
                        Box(
                            Modifier.size(26.dp).clip(CircleShape).border(2.dp, Ter, CircleShape)
                                .clickable { AppStore.setTaskStatus(task.id, "complete") },
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(Icons.Outlined.Check, contentDescription = "Выполнено: ${task.title}", tint = Ter, modifier = Modifier.size(14.dp))
                        }
                    }
                }
            }
        }
        Spacer(Modifier.height(16.dp))
    }

    val openedBooking = opened?.let { target -> bookings.firstOrNull { it.id == target.first } }
    if (openedBooking != null) BookingSheet(openedBooking, opened?.second, pages) { opened = null }
    if (posting) SlotsPostSheet(pages.orEmpty(), null) { posting = false }
}

/** Герой «следующая встреча» (.next): крупное время, кто и сколько, кнопки на карточке. */
@Composable
private fun NextCard(next: NextUp, now: LocalDateTime, color: Color, price: String, onOpen: () -> Unit, onWrite: () -> Unit) {
    val minutes = java.time.Duration.between(next.start, next.end).toMinutes()
    Box(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(28.dp))
            .background(color)
            .drawBehind {
                drawCircle(Color.White.copy(alpha = 0.12f), radius = 85.dp.toPx(),
                    center = Offset(size.width - 55.dp.toPx(), 45.dp.toPx()))
            }
            .clickable(onClick = onOpen)
            .padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = 14.dp),
    ) {
        Column {
            Row(Modifier.fillMaxWidth()) {
                Text("Следующая · ${untilLabel(next.start, next.end, now)}", color = Color.White.copy(alpha = 0.85f),
                    style = MaterialTheme.typography.labelMedium, modifier = Modifier.weight(1f))
                Text(next.booking.pageTitle, color = Color.White.copy(alpha = 0.85f), style = MaterialTheme.typography.labelMedium,
                    maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(start = 8.dp).weight(0.8f, fill = false))
            }
            Text(timeOf(next.occurrence.start), color = Color.White, fontSize = 46.sp, fontWeight = FontWeight.ExtraBold,
                letterSpacing = (-2).sp, lineHeight = 48.sp, modifier = Modifier.padding(top = 6.dp))
            Text(
                buildString {
                    append(next.booking.guestName.ifEmpty { "Гость" })
                    append(" · $minutes мин")
                    if (price.isNotEmpty()) append(" · $price")
                },
                color = Color.White, style = MaterialTheme.typography.titleSmall, modifier = Modifier.padding(top = 4.dp),
            )
            // Оплата онлайн — второстепенной строкой: значок статуса + «Депозит 1 000 ₽ · ждём оплату».
            val payment = next.booking.payment
            val payView = payment?.let { paymentStatusView(it.status) }
            if (payment != null) {
                Row(Modifier.padding(top = 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (payView != null) {
                        Icon(statusIconVector(payView.icon), contentDescription = null, tint = Color.White.copy(alpha = 0.85f),
                            modifier = Modifier.size(13.dp))
                        Spacer(Modifier.width(4.dp))
                    }
                    Text(paymentLine(payment), color = Color.White.copy(alpha = 0.85f), style = MaterialTheme.typography.bodySmall)
                }
            }
            val status = occurrenceStatus(next.booking, next.occurrence, now)
            if (status.label != "Записан(а)" && status.label != payView?.label) {
                Text(status.label, color = Color.White.copy(alpha = 0.85f), style = MaterialTheme.typography.bodySmall)
            }
            Row(Modifier.fillMaxWidth().padding(top = 14.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                HeroButton("Подробнее", Modifier.weight(1f), onOpen)
                HeroButton(if (next.booking.guestEmail.isNotEmpty()) "Написать" else "Все записи", Modifier.weight(1f), onWrite)
            }
        }
    }
}

/** Стеклянная кнопка на акцентной карточке. */
@Composable
private fun HeroButton(text: String, modifier: Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(19.dp)
    Box(
        modifier
            .height(38.dp)
            .clip(shape)
            .background(Color.White.copy(alpha = 0.2f))
            .border(1.dp, Color.White.copy(alpha = 0.35f), shape)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(text, color = Color.White, style = MaterialTheme.typography.labelMedium)
    }
}

@Composable
private fun EmptyNext(loading: Boolean, noPages: Boolean, onAction: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(28.dp)).background(Card).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text("Следующая встреча", style = MaterialTheme.typography.labelMedium, color = Sec)
        Text(if (loading) "Загружаем записи…" else "Предстоящих встреч нет", style = MaterialTheme.typography.titleLarge, color = Fg)
        Text(if (noPages) "Заведите страницу записи — клиенты будут записываться сами." else "Поделитесь ссылкой записи или постом со свободными окнами.",
            style = MaterialTheme.typography.bodySmall, color = Sec)
        if (!loading) GradientButton(if (noPages) "Создать страницу" else "Пост со свободными окнами", onAction,
            Modifier.padding(top = 8.dp), height = 40.dp)
    }
}

private val TIME_COL = 40.dp
private val GAP = 9.dp
private val CAPSULE = 42.dp

/**
 * Лента дня: время слева, цветные капсулы на пунктирной линии, справа — статус
 * значком (с подписью для доступности). Свободные окна — пунктирные капсулы с «+».
 */
@Composable
private fun Timeline(
    entries: List<DayEntry>,
    now: LocalDateTime,
    onOpen: (app.selfvoy.kit.Booking, String) -> Unit,
    onTask: (AppStore.Task) -> Unit,
    onOffer: (DayEntry.Free) -> Unit,
) {
    val dots = Ter
    Column(
        Modifier.fillMaxWidth().drawBehind {
            val x = (TIME_COL + GAP + CAPSULE / 2).toPx()
            drawLine(
                dots, Offset(x, 20.dp.toPx()), Offset(x, size.height - 20.dp.toPx()), strokeWidth = 2.dp.toPx(),
                cap = StrokeCap.Round, pathEffect = PathEffect.dashPathEffect(floatArrayOf(0.1f, 5.dp.toPx())),
            )
        },
    ) {
        entries.forEach { entry ->
            when (entry) {
                is DayEntry.Now -> NowLine(now)
                is DayEntry.Meeting -> {
                    val minutes = java.time.Duration.between(entry.start, entry.end).toMinutes()
                    TimelineRow(
                        time = timeOf(entry.occurrence.start),
                        capsule = { Capsule(entry.color) { Icon(if (entry.color == BrandBlue) Icons.AutoMirrored.Outlined.MenuBook else Icons.Outlined.Star, null, tint = Color.White, modifier = Modifier.size(21.dp)) } },
                        title = entry.booking.guestName.ifEmpty { "Гость" },
                        subtitle = "${entry.booking.pageTitle.ifEmpty { "Запись" }} · $minutes мин · ${entry.status.label.lowercase()}",
                        onClick = { onOpen(entry.booking, entry.occurrence.start) },
                    ) { StatusMark(entry.status) }
                }
                is DayEntry.TaskItem -> TimelineRow(
                    time = "%02d:%02d".format(entry.start.hour, entry.start.minute),
                    capsule = {
                        Capsule(FillSoft, onClick = { onTask(entry.task) }) {
                            Icon(Icons.Outlined.Check, contentDescription = "Выполнено: ${entry.task.title}", tint = Sec, modifier = Modifier.size(20.dp))
                        }
                    },
                    title = entry.task.title,
                    subtitle = "Задача",
                    onClick = null,
                ) {}
                is DayEntry.Free -> TimelineRow(
                    time = timeOf(entry.slot.start),
                    capsule = { FreeCapsule() },
                    title = "Свободно",
                    subtitle = "${timeOf(entry.slot.start)}–${timeOf(entry.slot.end)}",
                    muted = true,
                    onClick = null,
                ) {
                    Box(
                        Modifier.clip(RoundedCornerShape(50)).background(TintSoft).clickable { onOffer(entry) }
                            .padding(horizontal = 11.dp, vertical = 5.dp),
                    ) {
                        Text("Предложить", style = MaterialTheme.typography.labelMedium, color = Tint)
                    }
                }
            }
        }
    }
}

@Composable
private fun TimelineRow(
    time: String,
    capsule: @Composable () -> Unit,
    title: String,
    subtitle: String,
    onClick: (() -> Unit)?,
    muted: Boolean = false,
    trailing: @Composable () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 56.dp)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(time, style = MaterialTheme.typography.labelMedium, color = Sec, modifier = Modifier.width(TIME_COL),
            textAlign = androidx.compose.ui.text.style.TextAlign.End)
        Spacer(Modifier.width(GAP))
        capsule()
        Spacer(Modifier.width(GAP))
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = if (muted) Sec else Fg,
                fontWeight = if (muted) FontWeight.Medium else FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = Sec, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Spacer(Modifier.width(8.dp))
        trailing()
    }
}

@Composable
private fun Capsule(color: Color, onClick: (() -> Unit)? = null, content: @Composable () -> Unit) {
    Box(
        Modifier.size(CAPSULE).clip(RoundedCornerShape(15.dp)).background(color)
            .then(if (onClick != null) Modifier.clickable(onClick = onClick) else Modifier),
        contentAlignment = Alignment.Center,
    ) { content() }
}

/** Свободное окно: капсула с пунктирной рамкой и «+» (.row.free .cap). */
@Composable
private fun FreeCapsule() {
    val dash = Ter
    Box(
        Modifier.size(CAPSULE).clip(RoundedCornerShape(15.dp)).background(Bg).drawBehind {
            val stroke = 2.dp.toPx()
            drawRoundRect(
                dash, topLeft = Offset(stroke / 2, stroke / 2),
                size = androidx.compose.ui.geometry.Size(size.width - stroke, size.height - stroke),
                cornerRadius = CornerRadius(15.dp.toPx()),
                style = Stroke(width = stroke, pathEffect = PathEffect.dashPathEffect(floatArrayOf(5.dp.toPx(), 4.dp.toPx()))),
            )
        },
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Outlined.Add, contentDescription = null, tint = Sec, modifier = Modifier.size(20.dp))
    }
}

/** Красная линия «сейчас» с временем и точкой (.nowline). */
@Composable
private fun NowLine(now: LocalDateTime) {
    val red = Red
    Row(Modifier.fillMaxWidth().height(16.dp).padding(vertical = 1.dp), verticalAlignment = Alignment.CenterVertically) {
        Text("%02d:%02d".format(now.hour, now.minute), style = MaterialTheme.typography.labelSmall, color = red,
            fontWeight = FontWeight.Bold, modifier = Modifier.width(TIME_COL), textAlign = androidx.compose.ui.text.style.TextAlign.End)
        Spacer(Modifier.width(GAP))
        Box(
            Modifier.weight(1f).height(10.dp).drawBehind {
                val y = size.height / 2
                drawCircle(red, radius = 5.dp.toPx(), center = Offset(4.dp.toPx(), y))
                drawLine(red, Offset(0f, y), Offset(size.width, y), strokeWidth = 2.dp.toPx(), cap = StrokeCap.Round)
            },
        )
    }
}
