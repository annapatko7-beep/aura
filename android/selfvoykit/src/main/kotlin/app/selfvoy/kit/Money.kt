package app.selfvoy.kit

/*
 * Деньги: сервер хранит суммы в минимальных единицах валюты (копейках, центах)
 * целыми числами (docs/PROTOCOL.md → «Цена, предоплата и политика отмены»).
 * Показ («2 000 ₽», «2 000,50 ₽») и разбор ввода владельца («2 000,5» → 200050).
 * Зеркало web/src/lib/money.ts.
 */
object Money {

    data class Currency(val code: String, val label: String, val symbol: String)

    val CURRENCIES = listOf(
        Currency("RUB", "Рубли, ₽", "₽"),
        Currency("USD", "Доллары, $", "$"),
        Currency("EUR", "Евро, €", "€"),
        Currency("KZT", "Тенге, ₸", "₸"),
        Currency("BYN", "Белорусские рубли, Br", "Br"),
    )

    /** Наибольшая цена страницы и пакета на сервере. */
    const val PRICE_MAX = 100_000_000L

    /** Неразрывный пробел: сумма не переносится по строкам. */
    private const val NBSP = ' '

    fun symbol(currency: String): String =
        CURRENCIES.firstOrNull { it.code == currency }?.symbol ?: currency.ifEmpty { "₽" }

    private fun groups(n: Long): String {
        val digits = n.toString()
        val out = StringBuilder()
        digits.forEachIndexed { index, c ->
            if (index > 0 && (digits.length - index) % 3 == 0) out.append(NBSP)
            out.append(c)
        }
        return out.toString()
    }

    /** «2 000 ₽», «2 000,50 ₽», «15 $»: копейки — только если они есть. */
    fun format(minor: Long, currency: String = "RUB"): String {
        val negative = minor < 0
        val abs = kotlin.math.abs(minor)
        val whole = abs / 100
        val cents = abs % 100
        val body = groups(whole) + if (cents != 0L) "," + cents.toString().padStart(2, '0') else ""
        return (if (negative) "−" else "") + body + NBSP + symbol(currency.ifEmpty { "RUB" })
    }

    private val INPUT = Regex("^(\\d+)(?:[.,](\\d{0,2}))?$")

    /**
     * Ввод суммы в рублях (долларах…) → минимальные единицы. Пустая строка → 0.
     * Пробелы между разрядами и дробная часть через запятую или точку (до двух
     * знаков) допускаются; мусор, минус, лишние знаки → null.
     */
    fun parseInput(text: String): Long? {
        val value = text.filterNot { it.isWhitespace() || it == NBSP || it == ' ' }
            .removeSuffix("₽").removeSuffix("$").removeSuffix("€").removeSuffix("₸").removeSuffix("Br")
        if (value.isEmpty()) return 0
        val match = INPUT.matchEntire(value) ?: return null
        val whole = match.groupValues[1].toLongOrNull() ?: return null
        if (whole > Long.MAX_VALUE / 100 - 1) return null
        val cents = match.groupValues[2].padEnd(2, '0').ifEmpty { "00" }.toLong()
        return whole * 100 + cents
    }

    /** Значение для поля ввода: 200000 → «2000», 200050 → «2000,50», 0 → "". */
    fun inputValue(minor: Long): String {
        if (minor <= 0) return ""
        val whole = minor / 100
        val cents = minor % 100
        return if (cents != 0L) "$whole," + cents.toString().padStart(2, '0') else whole.toString()
    }
}
