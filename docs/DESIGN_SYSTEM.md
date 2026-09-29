# Дизайн-система Aura (full_mix)

Концепция «full_mix»: тёмный графит + стеклянные панели (glassmorphism) +
два цветных «сияния» на фоне + акцентный фиолетовый. Единый источник
токенов — `client/qml/theme/AuraTheme.qml` (QML-синглтон); iOS повторяет
палитру системными средствами SwiftUI. Полное описание дизайна —
[DESIGN.md](DESIGN.md).

## Токены

### Цвета фона
| токен | значение | назначение |
| --- | --- | --- |
| `bgDeep` | `#0D0D0F` | фон окна |
| `bgBase` | `#16161A` | верх градиента |
| `bgRaised` | `#1E1E24` | карточки, секции |
| `bgElevated` | `#26262E` | модалки, дропдауны |

### Стекло
| токен | значение | назначение |
| --- | --- | --- |
| `glassFill` / `glassFillHover` / `glassFillPressed` | 0.04 / 0.08 / 0.12 | прозрачность заполнения |
| `glassBorder` / `glassBorderHover` | 0.08 / 0.15 | прозрачность обводки |
| `glassHighlight` | 0.14 | верхний блик |
| `glassTint` | `#FFFFFF` | оттенок стекла |

### Акцент и статусы
| токен | значение |
| --- | --- |
| `accent` | `#7C6AFF` (фиолетовый) |
| `accentSoft` | `#A78BFA` (сообщения Ауры, подсветка) |
| `accentDeep` | `#5B4DD6` |
| `accentFill` | 0.20 |
| `accentGlow` | `#407C6AFF` (свечение кнопок) |
| `success` / `online` | `#4ADE80` |
| `warning` | `#FBBF24` |
| `danger` | `#F87171` |

### Текст
| токен | значение |
| --- | --- |
| `textPrimary` | `#F0F0F4` |
| `textSecondary` | `#9A9AA4` |
| `textMuted` | `#5C5C66` |
| `textOnAccent` | `#FFFFFF` |

### Типографика
Стек: `Inter, 'SF Pro Text', 'Segoe UI', 'Noto Sans', sans-serif`.
Размеры: `fontMicro` 11, `fontSmall` 12, `fontBody` 14, `fontTitle` 18,
`fontHero` 28.

### Радиусы, отступы, тень, анимация
- Радиусы: `radiusPill` 999, `radiusCard` 22, `radiusBubble` 22,
  `radiusField` 14, `radiusSmall` 14.
- Отступы: `spaceXs` 4, `spaceSm` 8, `spaceMd` 16, `spaceLg` 24, `spaceXl` 32.
- Тень: `#59000000`, blur 32, y 8.
- Анимация: `pressMs` 200, `moveMs` 250.

## Компоненты (QML, 11 штук)

| компонент | назначение |
| --- | --- |
| `GlassSurface` | размывание фона под стеклом (общий backdrop) |
| `GlassPanel` | стеклянная карточка/секция |
| `GlassButton` | кнопка: quiet / accent / danger |
| `GlassField` | текстовое поле |
| `AuraAvatar` | аватар с инициалом и индикатором online |
| `ChatBubble` | пузырь сообщения (своё/чужое/агент) |
| `ChatList` | список чатов (общий для стека и master-detail) |
| `ConfirmationPanel` | барьер подтверждения опасных операций |
| `TasksPanel` | список задач |
| `VoicePanel` | голосовой ввод (волна, состояние) |
| `NotificationsPanel` | «входящая» уведомлений + push-устройства (этап 13) |

Экраны: `Main` (роутер), `LoginPage`, `ChatsPage`, `ChatPage`,
`SettingsPage`, `DesktopShell` (широкая раскладка).

## Адаптивность (этап 12)

- `breakpointWide` 1120: шире — `DesktopShell` (рельс `railWidth` 76 +
  master-detail `masterWidth` 340); уже — стек ChatsPage → ChatPage.
- Рельс: Чаты / Задачи / Ждут (бейдж подтверждений) / Пуши (бейдж
  непрочитанных, этап 13) / Ещё.
- Выбранный чат переживает смену раскладки (состояние в `Main`).
- Геометрия окна сохраняется между запусками (Qt Settings).

## Правила

- Никаких «белых экранов-заглушек»: у каждого состояния есть UI
  (пусто/загрузка/ошибка).
- Цвет — только из токенов; хардкод цветов в компонентах запрещён.
- Контраст текста к фону — не ниже WCAG AA (проверено на палитре).
- Пиктограммы — юникод-глифы (без иконочных шрифтов и внешних ассетов).
