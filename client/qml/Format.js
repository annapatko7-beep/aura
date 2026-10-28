.pragma library

// Formatting helpers shared by the v3 screens (dates arrive from the server as ISO 8601 UTC).

var MONTHS_GEN = ["января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                  "сентября", "октября", "ноября", "декабря"]
var DAYS = ["воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота"]
var DAYS_ACC = ["воскресенье", "понедельник", "вторник", "среду", "четверг", "пятницу", "субботу"]
var DAYS_SHORT = ["Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"]

function parse(iso) {
    if (!iso) return null
    var d = new Date(iso)
    return isNaN(d.getTime()) ? null : d
}

function pad(n) { return (n < 10 ? "0" : "") + n }

function time(iso) {
    var d = parse(iso)
    return d ? pad(d.getHours()) + ":" + pad(d.getMinutes()) : ""
}

function sameDay(a, b) {
    return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate()
}

function startOfDay(d) { var x = new Date(d); x.setHours(0, 0, 0, 0); return x }
function addDays(d, n) { var x = new Date(d); x.setDate(x.getDate() + n); return x }

// "сегодня", "завтра", "в субботу", "14 октября"
function dayPhrase(iso, accusative) {
    var d = parse(iso)
    if (!d) return ""
    var now = new Date()
    if (sameDay(d, now)) return "сегодня"
    if (sameDay(d, addDays(now, 1))) return "завтра"
    if (d - now < 6 * 86400000 && d > now) return (accusative ? "в " + DAYS_ACC[d.getDay()] : DAYS[d.getDay()])
    return d.getDate() + " " + MONTHS_GEN[d.getMonth()]
}

// Отметка времени события в прошлом: "сегодня, 10:10", "вчера, 18:05", "3 октября, 09:00".
function stamp(iso) {
    var d = parse(iso)
    if (!d) return ""
    var now = new Date()
    var day = sameDay(d, now) ? "сегодня"
            : sameDay(d, addDays(now, -1)) ? "вчера"
            : d.getDate() + " " + MONTHS_GEN[d.getMonth()]
    return day + ", " + time(iso)
}

function dayTitle(iso) {
    var d = parse(iso)
    if (!d) return ""
    var s = DAYS[d.getDay()]
    return s.charAt(0).toUpperCase() + s.slice(1)
}

function dateLabel(d) { return d.getDate() + " " + MONTHS_GEN[d.getMonth()] }
function dayShort(d) { return DAYS_SHORT[d.getDay()] }

// Понедельник..воскресенье недели, в которую попадает d.
function weekOf(d) {
    var start = startOfDay(d)
    var shift = (start.getDay() + 6) % 7
    var monday = addDays(start, -shift)
    var out = []
    for (var i = 0; i < 7; ++i) out.push(addDays(monday, i))
    return out
}

function plural(n, one, few, many) {
    var m10 = n % 10, m100 = n % 100
    if (m10 === 1 && m100 !== 11) return one
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few
    return many
}

// "через 25 минут", "через 3 часа"
function until(iso) {
    var d = parse(iso)
    if (!d) return ""
    var min = Math.round((d - new Date()) / 60000)
    if (min < 0) return "уже началось"
    if (min < 60) return "через " + min + " " + plural(min, "минуту", "минуты", "минут")
    var h = Math.round(min / 60)
    if (h < 24) return "через " + h + " " + plural(h, "час", "часа", "часов")
    var days = Math.round(h / 24)
    return "через " + days + " " + plural(days, "день", "дня", "дней")
}

function firstName(full) {
    return (full || "").trim().split(/\s+/)[0] || ""
}

// Задача ближайшая по сроку: due_at, иначе remind_at.
function taskWhen(task) {
    return task.due_at || task.remind_at || ""
}

function escape(text) {
    return String(text === undefined || text === null ? "" : text)
        .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
}

// Текст уведомления для ленты: заголовок + деталь, без технических полей.
function notificationDetail(n) {
    if (n.kind === "login.new") return n.payload && n.payload.device ? "устройство " + n.payload.device : ""
    return n.body || ""
}

// Родительный падеж имени для «Selfvoy Алекса», «Selfvoy Марии» — простые правила для частых имён.
function genitive(name) {
    var n = (name || "").trim()
    if (!n) return ""
    var last = n.slice(-1).toLowerCase()
    var prev = n.slice(-2, -1).toLowerCase()
    if (last === "я") return n.slice(0, -1) + "и"
    if (last === "а") return n.slice(0, -1) + ("гкхжчшщ".indexOf(prev) >= 0 ? "и" : "ы")
    if (last === "й" || last === "ь") return n.slice(0, -1) + "я"
    if ("бвгджзклмнпрстфхцчшщ".indexOf(last) >= 0) return n + "а"
    return n
}

// Окно переговоров в часах от текущего момента: двое суток, до конца недели, две недели.
function windowHours(key) {
    var now = new Date()
    if (key === "weekend") {
        var end = addDays(startOfDay(now), (7 - now.getDay()) % 7 + 1)
        return Math.max(12, Math.round((end - now) / 3600000))
    }
    if (key === "tomorrow") return 48
    return 336
}

// «ближайшие 36 часов» / «ближайшие 7 дней» — окно переговоров для людей.
function windowPhrase(hours) {
    if (hours < 48) return "ближайшие " + hours + " " + plural(hours, "час", "часа", "часов")
    var days = Math.floor((hours + 12) / 24)
    return "ближайшие " + days + " " + plural(days, "день", "дня", "дней")
}

// Понятное описание инструмента; серверное — запасной вариант для новых инструментов.
// Короткое название инструмента для заголовков и подтверждений.
function toolTitle(tool) {
    return ({ send_message: "Сообщения", create_note: "Заметки", create_reminder: "Напоминания",
              send_email: "Письма", find_cafe: "Подбор мест", book_table: "Бронирование",
              check_calendar: "Календарь", suggest_time: "Подбор времени" })[tool] || tool || ""
}

// Теги места из каталога (coffee, gluten_free…) — по-русски.
var PLACE_TAGS = {
    coffee: "кофе",
    brunch: "бранчи",
    dinner: "ужины",
    vegan: "веганское меню",
    gluten_free: "без глютена",
    meat: "мясо",
    quiet: "тихо",
    workspace: "можно поработать"
}
function placeTags(tags) {
    return (tags || []).map(function (t) { return PLACE_TAGS[t] || String(t).replace(/_/g, " ") }).join(", ")
}

function toolDescription(tool, fallback) {
    var known = {
        send_message: "Писать сообщения вашим контактам",
        create_note: "Сохранять заметки в память Selfvoy",
        create_reminder: "Ставить напоминания",
        send_email: "Отправлять письма от вашего имени",
        find_cafe: "Подбирать место под питание, бюджет и дорогу обоих",
        book_table: "Бронировать столик в выбранном месте",
        check_calendar: "Смотреть, свободны ли вы в нужное время",
        suggest_time: "Предлагать время встречи"
    }
    return known[tool] || fallback || ""
}

// Люди из чатов (кроме меня), без повторов: основа экрана контактов и выбора собеседника.
function contactsFrom(chats, myId) {
    var seen = {}, out = []
    for (var i = 0; i < (chats || []).length; ++i) {
        var chat = chats[i]
        var members = chat.members || []
        for (var j = 0; j < members.length; ++j) {
            var m = members[j]
            if (m.user_id === myId || seen[m.user_id]) continue
            seen[m.user_id] = true
            out.push({ id: m.user_id, name: m.display_name || m.email, email: m.email, chatId: chat.id,
                       lastMessage: chat.last_message_body || "", lastAt: chat.last_message_at || "" })
        }
    }
    return out
}

function nameFor(target, chats, myId) {
    var list = contactsFrom(chats, myId)
    for (var i = 0; i < list.length; ++i)
        if (list[i].email === target || list[i].name === target) return list[i].name
    return target || ""
}

// ======================================================================= 2026: деньги
// Суммы приходят в минимальных единицах валюты (копейки) — docs/PROTOCOL.md, F4.
var CURRENCIES = [
    { value: "RUB", label: "Рубли, ₽", symbol: "₽" },
    { value: "USD", label: "Доллары, $", symbol: "$" },
    { value: "EUR", label: "Евро, €", symbol: "€" },
    { value: "KZT", label: "Тенге, ₸", symbol: "₸" },
    { value: "BYN", label: "Белорусские рубли, Br", symbol: "Br" }
]
var PRICE_MAX = 100000000

function currencySymbol(currency) {
    for (var i = 0; i < CURRENCIES.length; ++i) if (CURRENCIES[i].value === currency) return CURRENCIES[i].symbol
    return currency || "₽"
}

// «2 000 ₽», «2 000,50 ₽». Пробелы неразрывные.
function money(minor, currency) {
    if (typeof minor !== "number" || !isFinite(minor)) return ""
    var negative = minor < 0
    var abs = Math.round(Math.abs(minor))
    var whole = Math.floor(abs / 100)
    var cents = abs % 100
    var body = String(whole).replace(/\B(?=(\d{3})+(?!\d))/g, " ") + (cents ? "," + (cents < 10 ? "0" : "") + cents : "")
    return (negative ? "−" : "") + body + " " + currencySymbol(currency || "RUB")
}

// Ввод «2 000,5» → 200050 минимальных единиц; пусто → 0; мусор → -1.
function parseMoney(text) {
    var value = String(text || "").replace(/[\s ]/g, "").replace(/[₽$€₸]|Br$/, "")
    if (!value) return 0
    var m = /^(\d+)(?:[.,](\d{0,2}))?$/.exec(value)
    if (!m) return -1
    var cents = Number(((m[2] || "") + "00").slice(0, 2))
    var minor = Number(m[1]) * 100 + cents
    return minor <= Number.MAX_SAFE_INTEGER ? minor : -1
}

// 200000 → «2000», 200050 → «2000,50», 0 → "".
function moneyInput(minor) {
    if (!minor || minor <= 0) return ""
    var whole = Math.floor(minor / 100)
    var cents = Math.round(minor % 100)
    return cents ? whole + "," + (cents < 10 ? "0" : "") + cents : String(whole)
}

// ======================================================================= 2026: статусы
// Статус — всегда значок и слово: {label, tone, icon}; tone — green|amber|new|red|accent|"".
var PAYMENT = {
    pending:  { owner: "Ждём оплату",      tone: "amber", icon: "clock" },
    claimed:  { owner: "Проверьте оплату", tone: "amber", icon: "hand" },
    paid:     { owner: "Оплачено",         tone: "green", icon: "check" },
    refunded: { owner: "Возврат",          tone: "",      icon: "undo-2" },
    waived:   { owner: "Без оплаты",       tone: "",      icon: "minus" },
    expired:  { owner: "Не оплачено",      tone: "red",   icon: "x" }
}

function paymentStatus(status) {
    var v = PAYMENT[status]
    return v ? { label: v.owner, tone: v.tone, icon: v.icon } : null
}

function awaitingPayment(payment) {
    return !!payment && (payment.status === "pending" || payment.status === "claimed")
}

// Что владелец может отметить: ждём → «оплачено» / «без оплаты»; оплачено → «возврат».
function paymentActions(payment) {
    switch (payment ? payment.status : "") {
    case "pending": case "claimed": return ["paid", "waived"]
    case "paid": return ["refunded"]
    case "waived": case "expired": return ["paid"]
    default: return []
    }
}

var PAYMENT_ACTION = { paid: "Отметить оплату", waived: "Без оплаты", refunded: "Возврат" }

// Статус встречи в ленте дня: оплата → «Приду» → прошла → новая запись → оплачено → записан(а).
function occurrenceStatus(booking, occurrence, now) {
    now = now || new Date()
    if (booking.status === "cancelled" || occurrence.status === "cancelled") return { label: "Отменена", tone: "red", icon: "x" }
    if (awaitingPayment(booking.payment)) return paymentStatus(booking.payment.status)
    if (occurrence.confirmed_at) return { label: "Подтвердил(а)", tone: "green", icon: "check" }
    var end = parse(occurrence.end) || parse(occurrence.start)
    if (end && end.getTime() <= now.getTime()) return { label: "Прошла", tone: "", icon: "check" }
    var created = parse(booking.created_at)
    if (created && now.getTime() - created.getTime() < 24 * 3600000) return { label: "Новая запись", tone: "new", icon: "plus" }
    if (booking.payment && booking.payment.status === "paid") return { label: "Оплачено", tone: "green", icon: "check" }
    return { label: "Записан(а)", tone: "", icon: "calendar" }
}

// Статус записи целиком (список записей).
function bookingStatus(booking, now) {
    now = now || new Date()
    if (booking.status === "cancelled") {
        var by = booking.cancelled_by
        return { label: by === "owner" ? "Отменили вы" : by === "system" ? "Отменена: нет оплаты" : "Отменил гость", tone: "red", icon: "x" }
    }
    var occ = booking.occurrences || []
    for (var i = 0; i < occ.length; ++i) {
        var s = parse(occ[i].start)
        if (occ[i].status === "active" && s && s > now) return { label: "Предстоит", tone: "accent", icon: "calendar" }
    }
    return { label: "Прошла", tone: "", icon: "check" }
}

var PACKAGE_STATUS = {
    pending:   { label: "Ждёт оплаты",          tone: "amber", icon: "clock" },
    active:    { label: "Действует",             tone: "green", icon: "check" },
    used:      { label: "Занятия закончились",   tone: "",      icon: "minus" },
    expired:   { label: "Срок истёк",            tone: "red",   icon: "x" },
    cancelled: { label: "Отменён",               tone: "red",   icon: "x" }
}
function packageStatus(status) {
    var v = PACKAGE_STATUS[status]
    return v ? { label: v.label, tone: v.tone, icon: v.icon } : { label: status || "Пакет", tone: "", icon: "minus" }
}

function sessionsLabel(n) { return n + " " + plural(n, "занятие", "занятия", "занятий") }
function daysLabel(n) { return n + " " + plural(n, "день", "дня", "дней") }

// «Действует до 20 января · ещё 12 дней» / «90 дней с момента оплаты» / «Срок истёк 3 января».
function validityLabel(pkg, now) {
    now = now || new Date()
    var until = parse(pkg.valid_until)
    if (!until) return pkg.valid_days ? daysLabel(pkg.valid_days) + " с момента оплаты" : "Срок начнётся после оплаты"
    var date = dateLabel(until) + (until.getFullYear() !== now.getFullYear() ? " " + until.getFullYear() : "")
    var ms = until.getTime() - now.getTime()
    if (ms <= 0 || pkg.status === "expired") return "Срок истёк " + date
    var days = Math.ceil(ms / 86400000)
    return "Действует до " + date + " · ещё " + daysLabel(days)
}

// «8 занятий · 12 000 ₽ · 90 дней».
function offerSummary(offer, currency) {
    var parts = [sessionsLabel(offer.sessions)]
    if (offer.price) parts.push(money(offer.price, currency))
    parts.push(daysLabel(offer.valid_days || 90))
    return parts.join(" · ")
}

// ======================================================================= 2026: «Сегодня»
// «через 1 ч 13 мин», «идёт сейчас», «завтра в 10:00», «пт, 23 октября».
function untilText(start, now, end) {
    var minutes = Math.round((start.getTime() - now.getTime()) / 60000)
    if (minutes <= 0) return end && end.getTime() > now.getTime() ? "идёт сейчас" : "началась"
    if (sameDay(start, now)) {
        if (minutes < 60) return "через " + minutes + " мин"
        var h = Math.floor(minutes / 60), m = minutes % 60
        return "через " + h + " ч" + (m ? " " + m + " мин" : "")
    }
    if (sameDay(start, addDays(now, 1))) return "завтра в " + pad(start.getHours()) + ":" + pad(start.getMinutes())
    return DAYS_SHORT[start.getDay()].toLowerCase() + ", " + dateLabel(start)
}

function hhmm(d) { return pad(d.getHours()) + ":" + pad(d.getMinutes()) }

// Номер цвета страницы (1..5) — по порядку создания страниц.
function pageColor(pageId, pages) {
    var ids = (pages || []).map(function (p) { return p.id }).sort(function (a, b) { return a - b })
    var index = ids.indexOf(pageId)
    return ((index >= 0 ? index : Math.abs(pageId)) % 5) + 1
}

function pageById(pages, id) {
    for (var i = 0; i < (pages || []).length; ++i) if (pages[i].id === id) return pages[i]
    return null
}

// Ближайшая встреча: идущая сейчас или следующая.
function nextUp(bookings, now) {
    var best = null
    for (var i = 0; i < (bookings || []).length; ++i) {
        var b = bookings[i]
        if (b.status !== "active") continue
        var occ = b.occurrences || []
        for (var j = 0; j < occ.length; ++j) {
            if (occ[j].status !== "active") continue
            var start = parse(occ[j].start)
            var end = parse(occ[j].end) || start
            if (!start || !end || end.getTime() <= now.getTime()) continue
            if (!best || start.getTime() < best.start.getTime())
                best = { booking: b, occurrence: occ[j], start: start, end: end }
        }
    }
    return best
}

var FREE_IN_DAY = 3

/*
  Лента дня: встречи записей, задачи со сроком (кроме созданных записью), до
  FREE_IN_DAY свободных окон (будущие, не пересекаются со встречами) и линия
  «сейчас», если день — сегодня. Элементы: {kind: booking|task|free|now, …}.
*/
function buildDay(bookings, tasks, free, pages, day, now) {
    var entries = [], busy = [], bookingMinutes = {}
    for (var i = 0; i < (bookings || []).length; ++i) {
        var b = bookings[i]
        var occ = b.occurrences || []
        for (var j = 0; j < occ.length; ++j) {
            var s0 = parse(occ[j].start)
            if (s0) bookingMinutes[Math.floor(s0.getTime() / 60000)] = true
            if (!s0 || !sameDay(s0, day)) continue
            if (b.status !== "active" || occ[j].status !== "active") continue
            var e0 = parse(occ[j].end) || s0
            busy.push({ start: s0.getTime(), end: e0.getTime() })
            entries.push({ kind: "booking", key: "b" + b.id + "-" + occ[j].start, start: s0, end: e0,
                           booking: b, occurrence: occ[j], color: pageColor(b.page_id, pages),
                           status: occurrenceStatus(b, occ[j], now) })
        }
    }
    for (var t = 0; t < (tasks || []).length; ++t) {
        var task = tasks[t]
        if (task.status !== "pending") continue
        var at = parse(task.due_at) || parse(task.remind_at)
        if (!at || !sameDay(at, day)) continue
        if (bookingMinutes[Math.floor(at.getTime() / 60000)]) continue
        entries.push({ kind: "task", key: "t" + task.id, start: at, task: task })
    }
    var slots = (free || []).slice().sort(function (a, b) { return Date.parse(a.start) - Date.parse(b.start) })
    var count = 0, seen = {}
    for (var k = 0; k < slots.length && count < FREE_IN_DAY; ++k) {
        var s = parse(slots[k].start), e = parse(slots[k].end)
        if (!s || !e || !sameDay(s, day) || s.getTime() <= now.getTime() || seen[s.getTime()]) continue
        var clash = false
        for (var q = 0; q < busy.length; ++q) if (s.getTime() < busy[q].end && e.getTime() > busy[q].start) clash = true
        if (clash) continue
        seen[s.getTime()] = true
        busy.push({ start: s.getTime(), end: e.getTime() })
        entries.push({ kind: "free", key: "f" + slots[k].page_id + "-" + slots[k].start, start: s, end: e, pageId: slots[k].page_id })
        ++count
    }
    entries.sort(function (a, b) {
        return a.start.getTime() - b.start.getTime() || (a.kind === "free" ? 1 : 0) - (b.kind === "free" ? 1 : 0)
    })
    if (sameDay(day, now) && entries.length > 0) {
        var index = -1
        for (var n = 0; n < entries.length; ++n) if (entries[n].start.getTime() > now.getTime()) { index = n; break }
        var line = { kind: "now", key: "now", start: now }
        if (index === -1) entries.push(line)
        else entries.splice(index, 0, line)
    }
    return entries
}

/*
  Деньги записи: главное число — цена услуги, зафиксированная при записи
  (booking.price, минимальные единицы; 0 — по абонементу или бесплатно). Старый
  сервер без поля — цена страницы. payment.amount — только сумма оплаты онлайн
  (депозит или предоплата), её показываем отдельной строкой.
*/
var BY_PACKAGE = "По абонементу"

function hasBookingPrice(booking) {
    return !!booking && booking.price !== undefined && booking.price !== null
}

function bookingPrice(booking, page) {
    if (!booking || booking.package_id > 0) return 0
    if (hasBookingPrice(booking)) return Math.max(0, Number(booking.price) || 0)
    return page && page.price > 0 ? page.price : 0
}

function bookingCurrency(booking, page) {
    return (booking && booking.currency) || (page && page.currency) || "RUB"
}

// «3 000 ₽» · «По абонементу» · "" (бесплатно / цены нет).
function bookingPriceText(booking, page) {
    if (!booking) return ""
    if (booking.package_id > 0) return BY_PACKAGE
    var amount = bookingPrice(booking, page)
    return amount > 0 ? money(amount, bookingCurrency(booking, page)) : ""
}

// «Депозит 1 000 ₽» · «Предоплата 3 000 ₽» · «Оплата 3 000 ₽».
function paymentText(payment) {
    if (!payment) return ""
    var what = payment.kind === "deposit" ? "Депозит" : payment.kind === "full" ? "Предоплата" : "Оплата"
    return what + " " + money(payment.amount, payment.currency)
}

// «Депозит 1 000 ₽ · ждём оплату» — строка под ценой (рядом — значок статуса).
function paymentLine(payment) {
    if (!payment) return ""
    var status = paymentStatus(payment.status)
    if (!status) return paymentText(payment)
    return paymentText(payment) + " · " + status.label.charAt(0).toLowerCase() + status.label.slice(1)
}

// «3 встречи · 6 000 ₽» — встречи дня и их стоимость: цены записей без занятий
// по абонементу (оплачены пакетом); одна валюта — первая встреченная.
function daySummary(entries, pages) {
    var count = 0, amount = 0, currency = ""
    for (var i = 0; i < entries.length; ++i) {
        if (entries[i].kind !== "booking") continue
        ++count
        var booking = entries[i].booking
        if (booking.status !== "active" || booking.package_id > 0) continue
        var page = pageById(pages, booking.page_id)
        var price = bookingPrice(booking, page)
        if (price <= 0) continue
        var c = bookingCurrency(booking, page)
        if (currency && c !== currency) continue
        currency = c
        amount += price
    }
    var text = count ? count + " " + plural(count, "встреча", "встречи", "встреч") : "Встреч нет"
    return amount ? text + " · " + money(amount, currency) : text
}

/*
  История записи для панели владельца: запись → оплата → напоминания → «Приду» →
  отмена. Элементы {title, detail, tone: done|wait|fail|info}.
*/
function bookingHistory(booking, focus, now) {
    now = now || new Date()
    var steps = [{ title: "Записался(ась) по ссылке", detail: stamp(booking.created_at), tone: "done" }]
    var p = booking.payment
    if (p) {
        var amount = money(p.amount, p.currency)
        var kind = p.kind === "deposit" ? "депозит" : "оплату"
        if (p.status === "pending")
            steps.push({ title: "Ждём " + kind + " " + amount, detail: p.due_at ? "до " + stamp(p.due_at) + (p.method === "yookassa" ? " · ЮKassa" : " · по ссылке") : "", tone: "wait" })
        else if (p.status === "claimed")
            steps.push({ title: "Гость отметил: оплатил(а) " + amount, detail: "Проверьте поступление и отметьте оплату", tone: "wait" })
        else if (p.status === "paid")
            steps.push({ title: "Оплачено " + amount, detail: p.paid_at ? stamp(p.paid_at) : "", tone: "done" })
        else if (p.status === "waived")
            steps.push({ title: "Без оплаты", detail: "Вы отметили, что оплата не нужна", tone: "info" })
        else if (p.status === "refunded")
            steps.push({ title: "Возврат " + amount, detail: p.refunded_at ? stamp(p.refunded_at) : "", tone: "info" })
        else if (p.status === "expired")
            steps.push({ title: "Оплата не поступила к сроку", detail: p.due_at ? "срок был " + stamp(p.due_at) : "", tone: "fail" })
    }
    if (focus) {
        var reminders = (booking.reminders || []).filter(function (r) { return r.start === focus.start })
        reminders.sort(function (a, b) { return Date.parse(a.due_at) - Date.parse(b.due_at) })
        for (var i = 0; i < reminders.length; ++i) {
            var r = reminders[i]
            var what = r.kind === "24h" ? "за сутки" : "за 2 часа"
            if (r.sent_at) steps.push({ title: "Напоминание " + what + " отправлено", detail: stamp(r.sent_at), tone: "done" })
            else if (booking.status === "active" && focus.status === "active")
                steps.push({ title: "Напоминание " + what, detail: "запланировано на " + stamp(r.due_at), tone: "info" })
        }
        if (focus.confirmed_at) steps.push({ title: "Подтвердил(а): «Приду»", detail: stamp(focus.confirmed_at), tone: "done" })
        else if (booking.status === "active" && focus.status === "active" && Date.parse(focus.start) > now.getTime())
            steps.push({ title: "Ещё не подтвердил(а)", detail: "Кнопка «Приду» — в напоминании", tone: "info" })
        if (focus.status === "cancelled" && booking.status === "active") steps.push({ title: "Эта встреча отменена", detail: "", tone: "fail" })
    }
    if (booking.status === "cancelled") {
        var by = booking.cancelled_by
        steps.push({ title: by === "owner" ? "Вы отменили запись" : by === "system" ? "Запись отменена: оплата не поступила" : "Гость отменил запись",
                     detail: stamp(booking.updated_at), tone: "fail" })
    }
    return steps
}

// ======================================================================= 2026: тариф и уведомления
// «Pro до 12 ноября» / «Free»; null — сервер не прислал plan.
function planView(plan, now) {
    if (!plan || !plan.name) return null
    now = now || new Date()
    var until = parse(plan.until)
    var pro = plan.name === "pro" && (!until || until.getTime() > now.getTime())
    if (pro) return { label: until ? "Pro до " + dateLabel(until) : "Pro", pro: true,
                      hint: "Скрытая подпись, предоплата, пакеты и ЮKassa доступны" }
    return { label: "Free", pro: false,
             hint: plan.enforced ? "Функции Pro: скрыть подпись, предоплата, пакеты, ЮKassa"
                                 : "Пока идёт бета, функции Pro открыты всем" }
}

var PRO_REQUIRED_TEXT = "Это доступно на тарифе Pro. Пригласите коллегу в Selfvoy — вы оба получите месяц Pro."

// Значок и тон уведомления по виду (те же kind, что на сервере).
function notificationIcon(kind) {
    switch (kind) {
    case "task.due": return "clock"
    case "confirmation.requested": return "triangle-alert"
    case "a2a.proposal": return "handshake"
    case "login.new": return "log-out"
    case "twofactor.enabled": case "twofactor.disabled": return "shield"
    case "invite.accepted": case "booking.confirmed": return "circle-check"
    case "invite.declined": case "booking.cancelled": return "circle-x"
    case "booking.created": return "calendar-plus"
    case "booking.rescheduled": return "calendar-clock"
    case "booking.payment": case "package.payment": return "credit-card"
    case "package.purchased": case "package.low": return "ticket"
    case "referral.joined": return "user-plus"
    case "referral.reward": return "gift"
    default: return "bell"
    }
}
function notificationTone(n) {
    var p = n.payload || {}
    switch (n.kind) {
    case "booking.confirmed": case "referral.reward": case "invite.accepted": return "green"
    case "booking.cancelled": case "invite.declined": return "red"
    case "package.low": case "confirmation.requested": return "amber"
    case "booking.payment": case "package.payment":
        return p.status === "paid" ? "green" : p.status === "expired" ? "red" : "amber"
    case "booking.created": case "package.purchased": return "new"
    default: return ""
    }
}
