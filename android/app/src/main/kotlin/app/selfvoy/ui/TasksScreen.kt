package app.selfvoy.ui

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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.KeyboardArrowLeft
import androidx.compose.material.icons.automirrored.outlined.KeyboardArrowRight
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.DeleteOutline
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.FillSoft
import app.selfvoy.ui.theme.Card
import app.selfvoy.ui.theme.Ter
import app.selfvoy.ui.theme.Accent
import app.selfvoy.ui.theme.AccentSoft
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.BgElevated
import app.selfvoy.ui.theme.Danger
import app.selfvoy.ui.theme.Dot
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.GradEnd
import app.selfvoy.ui.theme.GradStart
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Lilac
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.Success
import app.selfvoy.ui.theme.TextMuted
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import app.selfvoy.ui.theme.Warning
import java.time.Duration
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneId

/**
 * Задачи (макет Tasks): неделя, задачи выбранного дня, «Дальше», «Выполнено»,
 * подсказка «до следующего события» и «+» для новой задачи (tasks.create с due_at).
 */
@Composable
fun TasksScreen() {
    val tasks by AppStore.tasks.collectAsState()
    var day by remember { mutableStateOf(LocalDate.now()) }
    var adding by remember { mutableStateOf(false) }
    val week = remember(day) { day.minusDays((day.dayOfWeek.value - 1).toLong()).let { m -> (0..6).map { m.plusDays(it.toLong()) } } }

    val open = tasks.filter { it.status == "pending" }.sortedBy { it.whenAt.ifEmpty { "9999" } }
    fun dateOf(task: AppStore.Task) = parseInstant(task.whenAt)?.toLocalDate()
    val onDay = open.filter { (dateOf(it) ?: LocalDate.now()) == day }
    val later = open.filter { dateOf(it)?.isAfter(day) == true }
    val done = tasks.filter { it.status == "done" }.take(6)
    val next = open.firstOrNull { parseInstant(it.whenAt)?.isAfter(LocalDateTime.now()) == true }

    Box(Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            ScreenHeader("Задачи") { GradientButton("Сегодня", { day = LocalDate.now() }, height = 40.dp) }

            Row(verticalAlignment = Alignment.CenterVertically) {
                RoundIconButton(Icons.AutoMirrored.Outlined.KeyboardArrowLeft, "Предыдущая неделя", { day = day.minusWeeks(1) }, size = 34.dp)
                Text("${dateLabel(week.first())} – ${dateLabel(week.last())}", style = MaterialTheme.typography.labelMedium,
                    color = TextPrimary, modifier = Modifier.padding(horizontal = 12.dp))
                RoundIconButton(Icons.AutoMirrored.Outlined.KeyboardArrowRight, "Следующая неделя", { day = day.plusWeeks(1) }, size = 34.dp)
            }

            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                week.forEach { d ->
                    val selected = d == day
                    val has = open.any { dateOf(it) == d }
                    Column(
                        Modifier.weight(1f).clip(RoundedCornerShape(14.dp))
                            .background(if (selected) Accent.copy(alpha = 0.2f) else Color.Transparent)
                            .border(1.dp, if (selected) Lilac.copy(alpha = 0.45f) else Color.Transparent, RoundedCornerShape(14.dp))
                            .clickable { day = d }.padding(vertical = 8.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(dayShort(d), style = MaterialTheme.typography.labelSmall, color = TextMuted)
                        Text("${d.dayOfMonth}", style = MaterialTheme.typography.titleLarge,
                            color = if (d == LocalDate.now()) Lilac else TextPrimary)
                        Dot(if (has) AccentSoft else Color.Transparent, 5.dp)
                    }
                }
            }

            // Подсказка Selfvoy: время до следующего события
            GlassCard(Modifier.fillMaxWidth()) {
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text("Подсказки Selfvoy", style = MaterialTheme.typography.titleLarge, color = TextPrimary)
                    Text(next?.let { "«${it.title}» ${untilText(it.whenAt)}" } ?: "Впереди ничего не запланировано.",
                        style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                    next?.let {
                        val minutes = parseInstant(it.whenAt)?.let { at -> Duration.between(LocalDateTime.now(), at).toMinutes() } ?: 0
                        val progress = (1f - minutes / 120f).coerceIn(0.05f, 1f)
                        Box(Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)).background(FillSoft)) {
                            Box(Modifier.fillMaxWidth(progress).height(6.dp).clip(RoundedCornerShape(3.dp))
                                .background(Brush.horizontalGradient(listOf(GradStart, GradEnd))))
                        }
                        Muted("Время до следующего события")
                    }
                }
            }

            Section(if (day == LocalDate.now()) "Сегодня" else "${dayShort(day)}, ${dateLabel(day)}", onDay, day, "Свободный день")
            Section("Дальше", later, day, "Больше ничего не запланировано")
            Section("Выполнено", done, day, "Пока ничего")
            Spacer(Modifier.height(80.dp))
        }

        RoundIconButton(Icons.Outlined.Add, "Новая задача", { adding = true }, accent = true, size = 60.dp,
            modifier = Modifier.align(Alignment.BottomEnd).padding(20.dp))
    }

    if (adding) AddTaskDialog(day) { adding = false }
}

@Composable
private fun Section(title: String, items: List<AppStore.Task>, day: LocalDate, empty: String) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(title, style = MaterialTheme.typography.headlineSmall, color = TextPrimary)
        if (items.isEmpty()) Muted(empty)
        items.forEach { TaskRow(it, day) }
    }
}

@Composable
private fun TaskRow(task: AppStore.Task, day: LocalDate) {
    val isDone = task.status == "done"
    val at = parseInstant(task.whenAt)
    val tone = when {
        isDone -> Success
        at != null && at.isBefore(LocalDateTime.now()) -> Danger
        at != null && Duration.between(LocalDateTime.now(), at).toMinutes() < 120 -> Warning
        else -> AccentSoft
    }
    val shape = RoundedCornerShape(16.dp)
    Row(
        Modifier.fillMaxWidth().alpha(if (isDone) 0.62f else 1f).height(56.dp).clip(shape)
            .background(Card)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(22.dp).clip(RoundedCornerShape(7.dp))
                .background(if (isDone) Accent else Color.Transparent)
                .border(1.5.dp, if (isDone) Accent else Ter, RoundedCornerShape(7.dp))
                .clickable { AppStore.setTaskStatus(task.id, if (isDone) "reopen" else "complete") },
            contentAlignment = Alignment.Center,
        ) { if (isDone) Icon(Icons.Outlined.Check, contentDescription = "Выполнено", tint = Color.White, modifier = Modifier.size(14.dp)) }
        Text(task.title, style = MaterialTheme.typography.titleSmall, color = TextPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis,
            textDecoration = if (isDone) TextDecoration.LineThrough else null, modifier = Modifier.weight(1f).padding(horizontal = 12.dp))
        if (at != null) {
            // День недели — только внутри показанной недели, иначе «11 октября».
            val d = at.toLocalDate()
            val weekStart = day.with(java.time.DayOfWeek.MONDAY)
            val inWeek = !d.isBefore(weekStart) && d.isBefore(weekStart.plusDays(7))
            Muted(if (d == day) timeOf(task.whenAt) else "${if (inWeek) dayShort(d) else dateLabel(d)}, ${timeOf(task.whenAt)}")
        }
        Icon(Icons.Outlined.DeleteOutline, contentDescription = "Удалить", tint = TextMuted,
            modifier = Modifier.padding(start = 8.dp).size(18.dp).clickable { AppStore.deleteTask(task.id) })
        Spacer(Modifier.size(8.dp))
        Box(Modifier.size(10.dp).clip(CircleShape).background(tone))
    }
}

@Composable
private fun AddTaskDialog(day: LocalDate, onClose: () -> Unit) {
    var title by remember { mutableStateOf("") }
    var time by remember { mutableStateOf("18:00") }
    AlertDialog(
        onDismissRequest = onClose,
        containerColor = BgElevated,
        title = { Text("Новая задача · ${dateLabel(day)}", style = MaterialTheme.typography.titleMedium) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                SelfvoyField(title, { title = it }, "Например, отправить питч-дек", label = "Что сделать")
                SelfvoyField(time, { time = it.take(5) }, "ЧЧ:ММ", label = "Время")
            }
        },
        confirmButton = {
            TextButton(enabled = title.isNotBlank(), onClick = {
                val (h, m) = time.split(":").let { (it.getOrNull(0)?.toIntOrNull() ?: 18) to (it.getOrNull(1)?.toIntOrNull() ?: 0) }
                val due = day.atTime(h.coerceIn(0, 23), m.coerceIn(0, 59)).atZone(ZoneId.systemDefault()).toInstant().toString()
                AppStore.createTask(title, dueAt = due)
                onClose()
            }) { Text("Добавить") }
        },
        dismissButton = { TextButton(onClick = onClose) { Text("Отмена") } },
    )
}
