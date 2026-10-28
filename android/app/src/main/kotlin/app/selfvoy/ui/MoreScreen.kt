package app.selfvoy.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Chat
import androidx.compose.material.icons.outlined.CardGiftcard
import androidx.compose.material.icons.outlined.CardMembership
import androidx.compose.material.icons.outlined.CheckBox
import androidx.compose.material.icons.outlined.ContentCopy
import androidx.compose.material.icons.outlined.Handshake
import androidx.compose.material.icons.outlined.Link
import androidx.compose.material.icons.outlined.Notifications
import androidx.compose.material.icons.outlined.People
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material.icons.outlined.WarningAmber
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.kit.Plan
import app.selfvoy.kit.ReferralInfo
import app.selfvoy.ui.theme.Amber
import app.selfvoy.ui.theme.BrandAmber
import app.selfvoy.ui.theme.CapsuleColors
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Green
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.ProBadge
import app.selfvoy.ui.theme.Red
import app.selfvoy.ui.theme.ScreenHeader
import app.selfvoy.ui.theme.Sec
import app.selfvoy.ui.theme.StatusIcon
import app.selfvoy.ui.theme.StatusPill
import app.selfvoy.ui.theme.StatusView
import app.selfvoy.ui.theme.Tint
import app.selfvoy.ui.theme.Tone

/**
 * «Ещё»: тариф (значок Pro и срок), награда за приглашения (месяц Pro обоим, F3)
 * и остальные разделы — абонементы, подключения, задачи, переговоры, контакты,
 * входящие, настройки.
 */
@Composable
fun MoreScreen() {
    val plan by AppStore.plan.collectAsState()
    val referral by AppStore.referral.collectAsState()
    val unread by AppStore.unread.collectAsState()
    val confirmations by AppStore.confirmations.collectAsState()
    val packages by AppStore.packages.collectAsState()

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        ScreenHeader("Ещё")

        PlanGroup(plan)
        ReferralGroup(referral)

        Group(head = "Работа") {
            val low = packages.orEmpty().count { it.status == "active" && it.sessionsLeft <= 1 }
            ListRow("Абонементы", if (low > 0) "У $low ${plural(low.toLong(), "клиента", "клиентов", "клиентов")} осталось 1 занятие или меньше" else "Пакеты занятий клиентов",
                Icons.Outlined.CardMembership, BrandAmber, onClick = { AppStore.selectTab(AppStore.Tab.PACKAGES) })
            GroupDivider()
            ListRow("Подключения", "Telegram, ЮKassa, календарь в телефоне", Icons.Outlined.Link, Tint,
                onClick = { AppStore.selectTab(AppStore.Tab.INTEGRATIONS) })
            GroupDivider()
            ListRow("Задачи", icon = Icons.Outlined.CheckBox, iconBackground = Green, onClick = { AppStore.selectTab(AppStore.Tab.TASKS) })
            GroupDivider()
            ListRow("Переговоры", "Ассистент договаривается о встрече с чужим Selfvoy", Icons.Outlined.Handshake, CapsuleColors[4],
                onClick = { AppStore.selectTab(AppStore.Tab.NEGOTIATIONS) })
            GroupDivider()
            ListRow("Контакты и чаты", icon = Icons.Outlined.People, iconBackground = CapsuleColors[3],
                onClick = { AppStore.selectTab(AppStore.Tab.CONTACTS) })
        }

        Group {
            ListRow("Входящие", icon = Icons.Outlined.Notifications, iconBackground = Red, value = if (unread > 0) "$unread" else "",
                onClick = { AppStore.selectTab(AppStore.Tab.NOTICES) })
            GroupDivider()
            ListRow("Ждут решения", "Опасные действия ассистента — только с вашего подтверждения", Icons.Outlined.WarningAmber, Amber,
                value = if (confirmations.isNotEmpty()) "${confirmations.size}" else "",
                onClick = { AppStore.selectTab(AppStore.Tab.CONFIRMATIONS) })
            GroupDivider()
            ListRow("Ассистент", icon = Icons.AutoMirrored.Outlined.Chat, iconBackground = Tint,
                onClick = { AppStore.selectTab(AppStore.Tab.SELFVOY) })
            GroupDivider()
            ListRow("Настройки", "Профиль, оформление, безопасность, сервер", Icons.Outlined.Settings, Color(0xFF8E8E93),
                onClick = { AppStore.selectTab(AppStore.Tab.SETTINGS) })
        }
        Spacer(Modifier.height(8.dp))
    }
}

/** «Pro до 12 ноября» / «Free» с пояснением; старый сервер без тарифа — ничего. */
@Composable
private fun PlanGroup(plan: Plan?) {
    if (plan == null) return
    val pro = plan.isPro()
    val until = plan.untilInstant()?.let { dateLabel(java.time.LocalDateTime.ofInstant(it, java.time.ZoneId.systemDefault()).toLocalDate()) }
    val label = when {
        pro && until != null -> "Pro до $until"
        pro -> "Pro"
        else -> "Free"
    }
    val hint = when {
        pro -> "Скрытая подпись, предоплата, ЮKassa и пакеты доступны"
        plan.enforced -> "Функции Pro: скрыть подпись, предоплата, ЮKassa, пакеты"
        else -> "Пока идёт бета, функции Pro открыты всем"
    }
    Group(head = "Тариф", foot = hint) {
        val subtitle = "Месяц Pro — за каждого коллегу, который заведёт Selfvoy"
        if (pro) {
            ListRow(label, subtitle, Icons.Outlined.CardGiftcard, Tint, titleBadge = { ProBadge() })
        } else {
            ListRow(label, subtitle, Icons.Outlined.CardGiftcard, Sec)
        }
    }
}

/** Приглашения: код и ссылка, сколько пришло и сколько месяцев Pro уже начислено. */
@Composable
private fun ReferralGroup(info: ReferralInfo?) {
    val context = LocalContext.current
    Group(
        head = "Пригласить коллегу",
        foot = "Коллега подтвердит email — вы оба получите ${info?.rewardDays ?: 30} дней Pro. Наград — до ${info?.rewardCap ?: 12}; регистрации со страниц записи тоже засчитываются.",
    ) {
        if (info == null) {
            ListRow("Загружаем код приглашения…")
            return@Group
        }
        Column(Modifier.fillMaxWidth().padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row {
                Column(Modifier.weight(1f)) {
                    Text("Ваш код", style = MaterialTheme.typography.bodySmall, color = Sec)
                    Text(info.code, style = MaterialTheme.typography.headlineSmall, color = Fg)
                }
                if (info.rewarded > 0) {
                    StatusPill(StatusView("+${info.rewarded} ${plural(info.rewarded.toLong(), "месяц", "месяца", "месяцев")} Pro", Tone.GREEN, StatusIcon.CHECK))
                }
            }
            if (info.hasLink) Text(info.link, style = MaterialTheme.typography.bodySmall, color = Fg, maxLines = 2)
            else Muted("Сервер не знает свой публичный адрес — делимся кодом: коллега введёт его при регистрации.")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GradientButton("Пригласить", { shareText(context, "Selfvoy", info.shareText) }, Modifier.weight(1f),
                    icon = Icons.Outlined.Share, height = 42.dp)
                GlassButton("Скопировать", {
                    copyText(context, info.link)
                    AppStore.notify("Скопировано")
                }, Modifier.weight(1f), icon = Icons.Outlined.ContentCopy, height = 42.dp)
            }
        }
        GroupDivider()
        ListRow("Пришли по приглашению", "подтвердили email: ${info.verified}", value = "${info.joined}")
        GroupDivider()
        ListRow("Начислено наград", if (info.rewardsLeft > 0) "можно получить ещё ${info.rewardsLeft}" else "получен максимум наград",
            value = "${info.rewarded} из ${info.rewardCap}")
        info.referrals.take(5).forEach { item ->
            GroupDivider()
            ListRow(item.displayName.ifEmpty { "Без имени" }) {
                StatusPill(if (item.verified) StatusView("С Selfvoy", Tone.GREEN, StatusIcon.CHECK) else StatusView("Ждёт подтверждения", Tone.NEUTRAL, StatusIcon.CLOCK))
            }
        }
    }
}
