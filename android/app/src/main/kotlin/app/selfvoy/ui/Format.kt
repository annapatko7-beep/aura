package app.selfvoy.ui

import java.time.Duration
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.time.format.TextStyle
import java.util.Locale

// Даты с сервера — ISO 8601 (UTC "...Z" или без зоны); показываем в локальном времени.

private val RU = Locale("ru")
private val TIME = DateTimeFormatter.ofPattern("HH:mm")

fun parseInstant(iso: String): LocalDateTime? {
    if (iso.isBlank()) return null
    return runCatching { LocalDateTime.ofInstant(Instant.parse(iso), ZoneId.systemDefault()) }
        .recoverCatching { LocalDateTime.parse(iso.removeSuffix("Z")) }
        .getOrNull()
}

fun timeOf(iso: String): String = parseInstant(iso)?.format(TIME) ?: ""

fun dayPhrase(iso: String): String {
    val date = parseInstant(iso)?.toLocalDate() ?: return ""
    val today = LocalDate.now()
    return when (date) {
        today -> "сегодня"
        today.plusDays(1) -> "завтра"
        else -> if (date.isAfter(today) && date.isBefore(today.plusDays(6)))
            date.dayOfWeek.getDisplayName(TextStyle.FULL, RU)
        else "${date.dayOfMonth} ${date.month.getDisplayName(TextStyle.FULL, RU)}"
    }
}

fun dayTitle(iso: String): String =
    parseInstant(iso)?.dayOfWeek?.getDisplayName(TextStyle.FULL_STANDALONE, RU)
        ?.replaceFirstChar { it.titlecase(RU) } ?: ""

fun dayShort(date: LocalDate): String = date.dayOfWeek.getDisplayName(TextStyle.SHORT_STANDALONE, RU)
    .replaceFirstChar { it.titlecase(RU) }

fun dateLabel(date: LocalDate): String = "${date.dayOfMonth} ${date.month.getDisplayName(TextStyle.FULL, RU)}"

fun plural(n: Long, one: String, few: String, many: String): String {
    val m10 = n % 10
    val m100 = n % 100
    return when {
        m10 == 1L && m100 != 11L -> one
        m10 in 2..4 && (m100 < 12 || m100 > 14) -> few
        else -> many
    }
}

fun untilText(iso: String): String {
    val at = parseInstant(iso) ?: return ""
    val minutes = Duration.between(LocalDateTime.now(), at).toMinutes()
    return when {
        minutes < 0 -> "уже началось"
        minutes < 60 -> "через $minutes ${plural(minutes, "минуту", "минуты", "минут")}"
        minutes < 24 * 60 -> (minutes / 60).let { "через $it ${plural(it, "час", "часа", "часов")}" }
        else -> (minutes / 1440).let { "через $it ${plural(it, "день", "дня", "дней")}" }
    }
}

fun greeting(): String = when (LocalDateTime.now().hour) {
    in 0..4 -> "Доброй ночи"
    in 5..11 -> "Доброе утро"
    in 12..17 -> "Добрый день"
    else -> "Добрый вечер"
}

/** Родительный падеж имени для «Selfvoy Алекса», «Selfvoy Марии». */
fun genitive(name: String): String {
    val n = name.trim()
    if (n.isEmpty()) return n
    val last = n.last().lowercaseChar()
    val prev = if (n.length > 1) n[n.length - 2].lowercaseChar() else ' '
    return when {
        last == 'я' -> n.dropLast(1) + "и"
        last == 'а' -> n.dropLast(1) + if (prev in "гкхжчшщ") "и" else "ы"
        last == 'й' || last == 'ь' -> n.dropLast(1) + "я"
        last in "бвгджзклмнпрстфхцчшщ" -> n + "а"
        else -> n
    }
}

fun firstName(full: String): String = full.trim().split(Regex("\\s+")).firstOrNull().orEmpty()

/** Окно переговоров в часах от текущего момента: двое суток, до конца недели, две недели. */
fun windowHours(key: String): Int {
    val now = LocalDateTime.now()
    return when (key) {
        "weekend" -> {
            val daysToMonday = (8 - now.dayOfWeek.value) % 7 + if (now.dayOfWeek.value == 1) 7 else 0
            val end = now.toLocalDate().plusDays(daysToMonday.toLong().coerceAtLeast(1)).atStartOfDay()
            Duration.between(now, end).toHours().toInt().coerceAtLeast(12)
        }
        "tomorrow" -> 48
        else -> 336
    }
}

/** «ближайшие 36 часов» / «ближайшие 7 дней» — окно переговоров для людей, а не в часах. */
fun windowPhrase(hours: Int): String =
    if (hours < 48) "ближайшие $hours ${plural(hours.toLong(), "час", "часа", "часов")}"
    else ((hours + 12) / 24).let { "ближайшие $it ${plural(it.toLong(), "день", "дня", "дней")}" }

/** Понятное описание инструмента; серверное — запасной вариант для новых инструментов. */
/** Теги места из каталога (coffee, gluten_free…) — по-русски. */
fun placeTags(tags: List<String>): String = tags.joinToString(", ") { tag ->
    when (tag) {
        "coffee" -> "кофе"
        "brunch" -> "бранчи"
        "dinner" -> "ужины"
        "vegan" -> "веганское меню"
        "gluten_free" -> "без глютена"
        "meat" -> "мясо"
        "quiet" -> "тихо"
        "workspace" -> "можно поработать"
        else -> tag.replace('_', ' ')
    }
}

fun toolDescription(tool: String, fallback: String): String = when (tool) {
    "send_message" -> "Писать сообщения вашим контактам"
    "create_note" -> "Сохранять заметки в память Selfvoy"
    "create_reminder" -> "Ставить напоминания"
    "send_email" -> "Отправлять письма от вашего имени"
    "find_cafe" -> "Подбирать место под питание, бюджет и дорогу обоих"
    "book_table" -> "Бронировать столик в выбранном месте"
    "check_calendar" -> "Смотреть, свободны ли вы в нужное время"
    "suggest_time" -> "Предлагать время встречи"
    else -> fallback
}
