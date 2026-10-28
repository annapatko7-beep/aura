# Design mockups v3 (референс редизайна Glassmorphism v3)

Источник: 5 high-fidelity макетов от пользователя (30.09.2026, см. `TODO_PRODUCTION_READY.md`).
Оригиналы PNG: `login.png`, `register.png`, `home.png`, `chat.png`, `tasks.png` — положить
в эту папку рядом с README (в чат приложения файлы передаются, но не сохраняются в git).

## Контракт экранов (сверять каждый экран перед «готово»)

### Login (`login.png`)
- Круглый светящийся лого-эмблема: кольцо из лучей, фиолетовое свечение, пульс
- Поля Email/Password с иконками (конверт, замок); глаз показа пароля
- Gradient-кнопка Sign in (violet→lavender) со свечением
- Continue with Google; ссылка Forgot password? · Create account

### Register (`register.png`)
- Фон: орбитальные эллипсы + blur-градиентные пятна
- Поля Full Name / Email / Password (глаз) / Timezone (dropdown) /
  «What should your Selfvoy help you with?»
- Gradient-кнопка Create account; Sign in with Google; Terms · Privacy

### Home (`home.png`)
- Hero-приветствие «Good evening, Alex» + статус «All systems operational»
- Карточка «Your Selfvoy is active» с фиолетовым свечением
- Строка «Ask your assistant to do something» + круглая фиолетовая кнопка микрофона + кнопка отправки
- Чипы быстрых действий (Plan a meeting / Check my schedule / Send an email / Create a reminder)
- Upcoming tasks с цветными точками статусов; Recent AI activity (таймлайн)

### Chat / My Selfvoy (`chat.png`)
- Шапка: аватар Selfvoy с фиолетовым градиентом-свечением, «Online · Personal assistant»
- Пузыри: свои — светлее стекло справа, Selfvoy — фиолетовое стекло слева
- Индикатор «Selfvoy is thinking…»; чипы быстрых ответов под сообщением Selfvoy

### Tasks (`tasks.png`)
- Недельная полоса дней (Mon–Sun) с задачами по дням
- Цветные точки-статусы справа в строках задач (pending/soon/done)
- Прогресс-бар «Time until next event»
- FAB «+» для создания задачи

### Contacts / Negotiations / Settings (по описанию TODO:385-416)
- Contacts: аватары агентов, плавающие сферы на фоне, детальная карточка справа
- Negotiations: две карточки агентов с анимированными звуковыми волнами,
  таймлайн раундов с номерами (лимит 5 → эскалация)
- Settings: lilac-индикатор активного пункта меню, карточки разрешений
  (Calendar/Email/Booking/…) с тумблерами

## Токены v3
См. `docs/DESIGN.md` (раздел «Палитра») — gradient CTA `#7C6AFF→#A78BFA`,
glow `rgba(124,106,255,.40)`, magenta-орбита `#C026D3`, ambient-подсветка панелей.
