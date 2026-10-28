package app.selfvoy.ui

import android.view.ViewTreeObserver
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.AutoAwesome
import androidx.compose.material.icons.outlined.CalendarToday
import androidx.compose.material.icons.outlined.CardMembership
import androidx.compose.material.icons.outlined.CheckBox
import androidx.compose.material.icons.outlined.EventAvailable
import androidx.compose.material.icons.outlined.Mic
import androidx.compose.material.icons.outlined.MoreHoriz
import androidx.compose.material.icons.outlined.Share
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.BrandAmber
import app.selfvoy.ui.theme.Fg
import app.selfvoy.ui.theme.Green
import app.selfvoy.ui.theme.Group
import app.selfvoy.ui.theme.GroupDivider
import app.selfvoy.ui.theme.InsetSheet
import app.selfvoy.ui.theme.ListRow
import app.selfvoy.ui.theme.Sec
import app.selfvoy.ui.theme.SelfvoyBackground
import app.selfvoy.ui.theme.Tint
import app.selfvoy.ui.theme.glass

/**
 * Оболочка после входа (язык 2026): текущий раздел и плавающая стеклянная панель
 * «Сегодня · Запись · ИИ · Ещё» с отдельной стеклянной кнопкой «+». Разделы из
 * «Ещё» и шапки «Сегодня» закрываются системной кнопкой «назад».
 */
@Composable
fun MainScaffold() {
    val tab by AppStore.tab.collectAsState()
    var creating by remember { mutableStateOf(false) }

    BackHandler(enabled = tab.root != tab) { AppStore.selectTab(tab.root) }

    SelfvoyBackground {
        Column(Modifier.fillMaxSize().statusBarsPadding()) {
            Box(Modifier.weight(1f).fillMaxWidth()) {
                when (tab) {
                    AppStore.Tab.HOME -> HomeScreen()
                    AppStore.Tab.BOOKING -> BookingScreen()
                    AppStore.Tab.SELFVOY -> MySelfvoyScreen()
                    AppStore.Tab.MORE -> MoreScreen()
                    AppStore.Tab.NEGOTIATIONS -> NegotiationsScreen()
                    AppStore.Tab.TASKS -> TasksScreen()
                    AppStore.Tab.CONTACTS -> ContactsScreen()
                    AppStore.Tab.SETTINGS -> SettingsScreen()
                    AppStore.Tab.CONFIRMATIONS -> ConfirmationsScreen()
                    AppStore.Tab.NOTICES -> NotificationsScreen()
                    AppStore.Tab.PACKAGES -> PackagesScreen()
                    AppStore.Tab.INTEGRATIONS -> IntegrationsScreen()
                }
            }
            // Пока открыта клавиатура, панель вкладок не нужна и не должна висеть над ней.
            if (!rememberImeVisible()) BottomBar(tab.root) { creating = true }
        }
    }

    if (creating) CreateSheet { creating = false }
}

private data class TabItem(val tab: AppStore.Tab, val label: String, val icon: ImageVector)

private val tabs = listOf(
    TabItem(AppStore.Tab.HOME, "Сегодня", Icons.Outlined.CalendarToday),
    TabItem(AppStore.Tab.BOOKING, "Запись", Icons.Outlined.EventAvailable),
    TabItem(AppStore.Tab.SELFVOY, "ИИ", Icons.Outlined.AutoAwesome),
    TabItem(AppStore.Tab.MORE, "Ещё", Icons.Outlined.MoreHoriz),
)

/*
  Плавающая стеклянная капсула вкладок и отдельный круглый «+» справа (.tabbar,
  .fab v2): отступы от краёв, под ними — фон приложения. Активная вкладка —
  акцентная «линза» и акцентный цвет значка и подписи.
*/
@Composable
private fun BottomBar(current: AppStore.Tab, onCreate: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.navigationBars)
            .padding(start = 14.dp, end = 14.dp, bottom = 10.dp, top = 6.dp)
            .height(62.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        val capsule = RoundedCornerShape(31.dp)
        Row(
            Modifier.weight(1f).fillMaxHeight().glass(capsule).padding(6.dp),
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            tabs.forEach { item ->
                val active = current == item.tab
                val lens = RoundedCornerShape(26.dp)
                Column(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .clip(lens)
                        .background(if (active) Tint.copy(alpha = 0.14f) else androidx.compose.ui.graphics.Color.Transparent)
                        .clickable { AppStore.selectTab(item.tab) },
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.Center,
                ) {
                    Icon(item.icon, contentDescription = null, tint = if (active) Tint else Sec, modifier = Modifier.size(23.dp))
                    Text(item.label, style = MaterialTheme.typography.labelSmall, color = if (active) Tint else Sec, maxLines = 1)
                }
            }
        }
        Box(
            Modifier.size(62.dp).glass(CircleShape).clickable(onClick = onCreate),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Outlined.Add, contentDescription = "Создать", tint = Fg, modifier = Modifier.size(28.dp))
        }
    }
}

/** «+»: быстрые действия владельца — запись, пост с окнами, абонемент, задача, голос. */
@Composable
private fun CreateSheet(onDismiss: () -> Unit) {
    fun run(action: () -> Unit) {
        onDismiss()
        action()
    }
    InsetSheet(onDismiss = onDismiss, title = "Создать") {
        Group {
            ListRow("Страница записи", "Ссылка, по которой клиенты записываются сами", Icons.Outlined.EventAvailable, Tint,
                onClick = { run { AppStore.startAction(AppStore.PendingAction.NEW_PAGE) } })
            GroupDivider()
            ListRow("Пост со свободными окнами", "Текст для Telegram, VK или сообщения", Icons.Outlined.Share, Green,
                onClick = { run { AppStore.startAction(AppStore.PendingAction.SLOTS_POST) } })
            GroupDivider()
            ListRow("Выдать абонемент", "Пакет занятий клиенту, проданный не на сайте", Icons.Outlined.CardMembership, BrandAmber,
                onClick = { run { AppStore.startAction(AppStore.PendingAction.ISSUE_PACKAGE) } })
            GroupDivider()
            ListRow("Задача", "Напоминание себе", Icons.Outlined.CheckBox, Sec,
                onClick = { run { AppStore.selectTab(AppStore.Tab.TASKS) } })
            GroupDivider()
            ListRow("Сказать ассистенту", "Голосом: «перенеси Сергея на пятницу»", Icons.Outlined.Mic, app.selfvoy.ui.theme.CapsuleColors[4],
                onClick = { run { AppStore.requestVoice() } })
        }
    }
}

/*
  Видна ли клавиатура — по insets корневого окна. WindowInsets.isImeVisible из
  Compose без edge-to-edge (decorFitsSystemWindows) не получает настоящих insets
  и считал клавиатуру открытой постоянно: панель вкладок пропадала после входа.
*/
@Composable
private fun rememberImeVisible(): Boolean {
    val view = LocalView.current
    var visible by remember { mutableStateOf(false) }
    DisposableEffect(view) {
        val listener = ViewTreeObserver.OnGlobalLayoutListener {
            visible = ViewCompat.getRootWindowInsets(view)?.isVisible(WindowInsetsCompat.Type.ime()) == true
        }
        view.viewTreeObserver.addOnGlobalLayoutListener(listener)
        listener.onGlobalLayout()
        onDispose { view.viewTreeObserver.removeOnGlobalLayoutListener(listener) }
    }
    return visible
}
