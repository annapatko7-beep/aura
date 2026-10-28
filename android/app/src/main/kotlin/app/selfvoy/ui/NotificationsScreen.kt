package app.selfvoy.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.ArrowBack
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.kit.SelfvoyNoticeKind
import app.selfvoy.ui.theme.Dot
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.RoundIconButton
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.SectionTitle
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary
import app.selfvoy.ui.theme.statusColor

/** Входящая: напоминания, переговоры, подтверждения, новые входы; push-устройства. */
@Composable
fun NotificationsScreen() {
    val notices by AppStore.notices.collectAsState()
    val devices by AppStore.pushDevices.collectAsState()
    val pushStatus by AppStore.pushStatus.collectAsState()
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = androidx.compose.foundation.layout.PaddingValues(20.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        // «Прочитать все» — в строке «назад»: рядом с заголовком 38 sp ему не хватает места.
        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                RoundIconButton(Icons.AutoMirrored.Outlined.ArrowBack, "Назад", { AppStore.selectTab(AppStore.Tab.HOME) }, size = 40.dp)
                Spacer(Modifier.weight(1f))
                if (notices.any { !it.read }) GlassButton("Прочитать все", { AppStore.markAllRead() }, height = 38.dp)
            }
        }
        item { ScreenHeader("Уведомления") }
        if (notices.isEmpty()) item { Muted("Пока пусто.") }
        items(notices, key = { it.id }) { notice ->
            GlassCard(Modifier.fillMaxWidth(), contentPadding = androidx.compose.foundation.layout.PaddingValues(16.dp),
                onClick = {
                    if (!notice.read) AppStore.markRead(notice.id)
                    noticeTarget(notice.kind)?.let { AppStore.selectTab(it) }
                }) {
                Row(verticalAlignment = Alignment.Top) {
                    Dot(if (notice.read) TextSecondary.copy(alpha = 0.3f) else statusColor(notice.kind), 9.dp)
                    Spacer(Modifier.width(12.dp))
                    Column(Modifier.weight(1f)) {
                        Text(notice.title, style = MaterialTheme.typography.titleSmall, color = TextPrimary)
                        val detail = if (notice.kind == "login.new") "" else notice.body
                        if (detail.isNotEmpty()) Text(detail, style = MaterialTheme.typography.bodySmall, color = TextSecondary)
                        Muted("${dayPhrase(notice.createdAt)}, ${timeOf(notice.createdAt)}")
                    }
                }
            }
        }
        item { SectionTitle("Push-устройства", Modifier.fillMaxWidth()) }
        item { Muted(pushStatus.ifEmpty { "Push ещё не регистрировался" }) }
        items(devices, key = { it.id }) { device ->
            GlassCard(Modifier.fillMaxWidth(), contentPadding = androidx.compose.foundation.layout.PaddingValues(14.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("${device.platform} · ${if (device.enabled) "включено" else "выключено"}",
                        style = MaterialTheme.typography.bodyMedium, color = TextPrimary, modifier = Modifier.weight(1f))
                    GlassButton("Отозвать", { AppStore.revokePushDevice(device.id) }, height = 36.dp, quiet = true)
                }
            }
        }
    }
}

/** Куда ведёт уведомление: записи — в «Запись», пакеты — в «Абонементы», награды — в «Ещё». */
private fun noticeTarget(kind: String): AppStore.Tab? = when {
    SelfvoyNoticeKind.touchesPackages(kind) -> AppStore.Tab.PACKAGES
    SelfvoyNoticeKind.touchesBookings(kind) -> AppStore.Tab.BOOKING
    SelfvoyNoticeKind.touchesReferral(kind) -> AppStore.Tab.MORE
    else -> null
}
