# Дизайн-система Selfvoy (v3)

Токены одни на все клиенты; описание дизайна — [DESIGN.md](DESIGN.md).
Эталон — `client/qml/theme/SelfvoyTheme.qml`; Android — `ui/theme/Theme.kt`,
iOS — `Support/Theme.swift`, веб — `preview/styles.css`.

## Цвета

| токен | значение | назначение |
| --- | --- | --- |
| `bgDeep` | `#09080E` | фон окна |
| `bgBase` | `#0F0D17` | низ градиента фона |
| `bgPanel` | `#14111D` | подложка стеклянных панелей |
| `bgRaised` | `#16131F` | карточки, секции |
| `bgElevated` | `#1E1A2B` | модалки, листы, дропдауны |
| `glassFill` / `Hover` / `Pressed` | 0,055 / 0,09 / 0,13 | заливка белым |
| `glassBorder` / `Hover` | 0,10 / 0,18 | обводка белым |
| `glassHighlight` | 0,14 | верхний блик |
| `glassEdge` | `#D9B8FF` | сиреневый блик по краю |
| `accent` | `#8B5CF6` | акцент |
| `accentSoft` | `#A78BFA` | Selfvoy, подсветка |
| `accentDeep` | `#6D28D9` | нажатое состояние |
| `accentGlow` | `#738B5CF6` | свечение (45 %) |
| `gradStart` / `gradMid` / `gradEnd` | `#E6B3FF` / `#A77BFF` / `#7346F5` | CTA |
| `glowViolet` / `glowMagenta` / `glowIndigo` | `#6D28D9` / `#4C1D95` / `#3B3CC4` | орбиты фона |
| `lilac` | `#D9B8FF` | индикатор, ссылки, курсор |
| `pink` | `#E8A8FF` | дополнительный акцент |
| `textPrimary` | `#F4F1FB` | основной текст |
| `textSecondary` | `#B9B3C9` | вторичный текст |
| `textMuted` | `#7D768F` | подписи, disabled |
| `textOnAccent` | `#FFFFFF` | текст на CTA |
| `success` / `online` | `#4ADE80` | |
| `warning` | `#FBBF24` | |
| `danger` | `#F87171` | |

## Типографика

Manrope (SIL OFL 1.1) встроен во все клиенты: Regular, Medium, SemiBold,
Bold, ExtraBold. Размеры: `fontMicro` 11, `fontSmall` 13, `fontBody` 15,
`fontLead` 18, `fontTitle` 22, `fontHero` 34, `fontDisplay` 54 (desktop);
на телефоне дисплей приветствия 46, заголовок экрана 38.

## Геометрия, тень, анимация

- Радиусы: `radiusPill` 999, `radiusShell` 32, `radiusCard` 24,
  `radiusBubble` 20, `radiusField` 14, `radiusSmall` 12.
- Отступы: 4 / 8 / 16 / 24 / 32 / 48.
- Тень: `#99000000`, blur 40, y 24.
- Анимация: `pressMs` 180, `moveMs` 250, `slowMs` 350, `pressScale` 0,98,
  кривая OutCubic.
- Desktop: `breakpointWide` 1000, `sidebarWidth` 258, рельс 84.

## Компоненты

| QML (`client/qml/components`) | Compose (`ui/theme/Components.kt`) | SwiftUI (`Views/Components.swift`) |
| --- | --- | --- |
| `SelfvoyBackground` | `SelfvoyBackground` | `SelfvoyBackground` |
| `GlassSurface`, `GlassPanel` | `GlassCard` | `glassCard()` |
| `GlassButton` (accent / glass / quiet / danger) | `GradientButton`, `GlassButton`, `RoundIconButton` | `GradientButton`, `GlassButton`, `RoundIconButton` |
| `GlassField`, `GlassComboBox` | `SelfvoyField` | `SelfvoyField` |
| `Chip` | `SelfvoyChip` | `SelfvoyChip` |
| `Bubble`, `ChatBubble` | `Bubble` | `Bubble` |
| `SelfvoyAvatar`, `AgentCard` | `SelfvoyAvatar` | `SelfvoyAvatar`, `AgentCard` |
| `SelfvoyEmblem`, `SelfvoyOrb` | `SelfvoyEmblem`, `SelfvoyOrb` | `SelfvoyEmblem`, `SelfvoyOrb` |
| `Icon` (Lucide, ISC) | Material Icons | SF Symbols |
| `LinkText` | `Link` | `LinkButton` |
| `ConfirmationPanel`, `NotificationsPanel`, `VoicePanel` | экраны подтверждений, уведомлений, `VoiceSheet` | `ConfirmationsView`, `NotificationsView`, `VoiceOverlay` |

## Правила

- Цвет — только из токенов; новый цвет сначала добавляется во все файлы токенов.
- У каждого состояния есть UI: пусто, загрузка, ошибка.
- Опасные действия (письмо, сообщение, бронь) — только по кнопке человека.
- Контраст основного и вторичного текста к фону — не ниже WCAG AA; `textMuted` (4,3:1) — только для подписей.
