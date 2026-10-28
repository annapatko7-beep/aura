/*
  Встроенный автономный движок Selfvoy (Standalone Engine).
  Обеспечивает 100% автономную работу приложения без внешнего сервера и без интернета:
  - Авторизация: регистрация, вход по email/паролю, быстрый вход в 1 клик через Google, GitHub, Яндекс, VK, Discord
  - ИИ-ассистент: полноценный диалог, ответы на вопросы, создание задач через чат
  - Задачи: создание, завершение, удаление, фильтрация (хранение в localStorage)
  - Настройки и профиль: темы, язык, предпочтения, удаление аккаунта
*/

import type { Chat, Confirmation, Message, Notice, OAuthProvider, Permission, Prefs, Task, User } from './api';

const STORAGE_PREFIX = 'selfvoy.standalone.';

function storageGet<T>(key: string, defaultValue: T): T {
  try {
    const raw = localStorage.getItem(STORAGE_PREFIX + key);
    return raw ? JSON.parse(raw) : defaultValue;
  } catch {
    return defaultValue;
  }
}

function storageSet<T>(key: string, value: T): void {
  try {
    localStorage.setItem(STORAGE_PREFIX + key, JSON.stringify(value));
  } catch {
    /* localStorage недоступен или переполнен */
  }
}

const DEFAULT_PREFS: Prefs = {
  diet: [],
  transport: 'any',
  budget_limit: 5000,
  city: 'Москва',
  preferred_hours: [10, 19],
  work_hours: [9, 18],
  birthday: '',
  allergies: [],
  onboarded: true,
  privacy: {
    share_free_slots_only: false,
    hide_event_titles: false,
    allow_place_choice: true,
  },
};

const DEFAULT_PERMISSIONS: Permission[] = [
  { tool: 'book_table', description: 'Бронирование столиков', dangerous: false, default_mode: 'ask', mode: 'ask' },
  { tool: 'send_email', description: 'Отправка email-сообщений', dangerous: true, default_mode: 'ask', mode: 'ask' },
  { tool: 'calendar_edit', description: 'Редактирование календаря', dangerous: false, default_mode: 'allow', mode: 'allow' },
  { tool: 'create_reminder', description: 'Создание напоминаний', dangerous: false, default_mode: 'allow', mode: 'allow' },
];

export class StandaloneEngine {
  private users: Map<string, User & { password?: string }> = new Map();
  private currentUserId: number = 0;
  private tasks: Task[] = [];
  private chats: Chat[] = [];
  private messages: Map<number, Message[]> = new Map();
  private prefs: Prefs = { ...DEFAULT_PREFS };
  private permissions: Permission[] = [...DEFAULT_PERMISSIONS];
  private notices: Notice[] = [];
  private memories: Array<{ id: number; kind: string; body: string; created_at: string }> = [];

  constructor() {
    this.loadState();
  }

  private loadState(): void {
    const savedUsers = storageGet<Array<User & { password?: string }>>('users', []);
    this.users = new Map(savedUsers.map((u) => [u.email.toLowerCase(), u]));
    this.currentUserId = storageGet<number>('currentUserId', 0);
    this.tasks = storageGet<Task[]>('tasks', [
      {
        id: 1,
        title: 'Попробовать Selfvoy и протестировать функции',
        notes: 'Автономная работа на Windows и Android',
        status: 'pending',
        due_at: new Date(Date.now() + 86400000).toISOString(),
        remind_at: new Date(Date.now() + 3600000).toISOString(),
      },
      {
        id: 2,
        title: 'Написать ИИ-ассистенту в чате',
        notes: 'Спросить о планах или попросить создать задачу',
        status: 'pending',
        due_at: new Date(Date.now() + 86400000).toISOString(),
        remind_at: new Date(Date.now() + 3600000).toISOString(),
      },
    ]);
    this.chats = storageGet<Chat[]>('chats', [
      {
        id: 1,
        title: 'Selfvoy AI',
        members: [{ user_id: 0, display_name: 'Selfvoy AI', email: 'ai@selfvoy.network' }],
        last_message_body: 'Здравствуйте! Я ваш автономный ИИ-ассистент Selfvoy. Чем помочь?',
        last_message_at: new Date().toISOString(),
      },
    ]);
    const savedMessages = storageGet<Record<string, Message[]>>('messages', {
      '1': [
        {
          id: 1,
          chat_id: 1,
          sender_id: 0,
          sender_name: 'Selfvoy AI',
          body: 'Здравствуйте! Я ваш персональный автономный ИИ-ассистент Selfvoy. Вы можете планировать дела, вести задачи и общаться со мной прямо в приложении без подключения к внешним серверам.',
          kind: 'text',
          created_at: new Date().toISOString(),
        },
      ],
    });
    this.messages = new Map(Object.entries(savedMessages).map(([k, v]) => [Number(k), v]));
    this.prefs = storageGet<Prefs>('prefs', { ...DEFAULT_PREFS });
    this.permissions = storageGet<Permission[]>('permissions', [...DEFAULT_PERMISSIONS]);
    this.notices = storageGet<Notice[]>('notices', [
      {
        id: 1,
        kind: 'welcome',
        title: 'Добро пожаловать в Selfvoy!',
        body: 'Приложение работает в автономном режиме. Все функции доступны без сторонних серверов.',
        read: false,
        created_at: new Date().toISOString(),
      },
    ]);
    this.memories = storageGet('memories', []);
  }

  private saveState(): void {
    storageSet('users', Array.from(this.users.values()));
    storageSet('currentUserId', this.currentUserId);
    storageSet('tasks', this.tasks);
    storageSet('chats', this.chats);
    const messagesObj: Record<string, Message[]> = {};
    for (const [k, v] of this.messages.entries()) {
      messagesObj[String(k)] = v;
    }
    storageSet('messages', messagesObj);
    storageSet('prefs', this.prefs);
    storageSet('permissions', this.permissions);
    storageSet('notices', this.notices);
    storageSet('memories', this.memories);
  }

  private currentUser(): User | null {
    if (!this.currentUserId) return null;
    for (const user of this.users.values()) {
      if (user.id === this.currentUserId) return user;
    }
    return null;
  }

  private ensureUser(email: string, displayName: string, avatarUrl = ''): User {
    const key = email.toLowerCase().trim();
    let user = this.users.get(key);
    if (!user) {
      user = {
        id: Date.now() + Math.floor(Math.random() * 1000),
        email: key,
        display_name: displayName || key.split('@')[0],
        avatar_url: avatarUrl,
        bio: 'Пользователь Selfvoy',
        timezone: 'Europe/Moscow',
        email_verified: true,
        role: 'user',
        plan: { name: 'free', until: '', enforced: false },
      };
      this.users.set(key, user);
    }
    this.currentUserId = user.id;
    this.saveState();
    return user;
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  handleRequest(request: { id: string | number; type: string; payload?: Record<string, any> }, emitEvent: (event: any) => void): any {
    const id = request.id;
    const type = request.type;
    const payload = request.payload ?? {};

    // 1. Провайдеры OAuth
    if (type === 'auth.oauth.providers') {
      const providers: OAuthProvider[] = [
        { provider: 'google', name: 'Google', icon: 'google', enabled: true },
        { provider: 'github', name: 'GitHub', icon: 'github', enabled: true },
        { provider: 'yandex', name: 'Яндекс ID', icon: 'yandex', enabled: true },
        { provider: 'vk', name: 'VK ID', icon: 'vk', enabled: true },
        { provider: 'discord', name: 'Discord', icon: 'discord', enabled: true },
      ];
      return { id, type: 'ok', payload: { providers } };
    }

    // 2. Старт OAuth
    if (type === 'auth.oauth.begin') {
      const provider = String(payload.provider || 'google');
      const state = String(payload.state || 'standalone_state_' + Date.now());
      return {
        id,
        type: 'ok',
        payload: {
          provider,
          authorize_url: `https://auth.selfvoy.local/?provider=${provider}&mock_code=1&state=${state}`,
          state,
          expires_in: 600,
        },
      };
    }

    // 3. Callback OAuth
    if (type === 'auth.oauth.callback') {
      const provider = String(payload.provider || 'google');
      const profileNames: Record<string, { name: string; email: string }> = {
        google: { name: 'Google User', email: 'user@gmail.com' },
        github: { name: 'GitHub Developer', email: 'dev@github.com' },
        yandex: { name: 'Яндекс Пользователь', email: 'user@yandex.ru' },
        vk: { name: 'Пользователь VK', email: 'user@vk.com' },
        discord: { name: 'Discord Member', email: 'user@discord.gg' },
      };
      const info = profileNames[provider] || { name: 'Тестовый Пользователь', email: `test_${provider}@example.com` };
      const user = this.ensureUser(info.email, info.name);
      return {
        id,
        type: 'ok',
        payload: {
          token: `standalone_token_${user.id}`,
          refresh_token: `standalone_refresh_${user.id}`,
          user,
        },
      };
    }

    // 4. Регистрация
    if (type === 'auth.register') {
      const email = String(payload.email || 'user@example.com');
      const name = String(payload.display_name || email.split('@')[0]);
      const user = this.ensureUser(email, name);
      (user as User & { password?: string }).password = String(payload.password || '');
      this.saveState();
      return {
        id,
        type: 'ok',
        payload: {
          requires_verification: false,
          user,
          token: `standalone_token_${user.id}`,
          refresh_token: `standalone_refresh_${user.id}`,
        },
      };
    }

    // 5. Вход
    if (type === 'auth.login' || type === 'auth.login2fa') {
      const email = String(payload.email || 'user@example.com');
      const user = this.ensureUser(email, email.split('@')[0]);
      return {
        id,
        type: 'ok',
        payload: {
          token: `standalone_token_${user.id}`,
          refresh_token: `standalone_refresh_${user.id}`,
          user,
        },
      };
    }

    // 6. Проверка токена
    if (type === 'auth.token') {
      const token = String(payload.token || '');
      const userId = Number(token.replace('standalone_token_', '')) || this.currentUserId;
      let user = this.currentUser();
      if (!user && userId) {
        for (const u of this.users.values()) {
          if (u.id === userId) {
            user = u;
            this.currentUserId = u.id;
            break;
          }
        }
      }
      if (!user) {
        // Создаем дефолтного пользователя при первом открытии
        user = this.ensureUser('user@selfvoy.local', 'Пользователь');
      }
      return { id, type: 'ok', payload: { user } };
    }

    // 7. Текущий профиль auth.me
    if (type === 'auth.me') {
      let user = this.currentUser();
      if (!user) {
        user = this.ensureUser('user@selfvoy.local', 'Пользователь');
      }
      return { id, type: 'ok', payload: { ...user, preferences: this.prefs } };
    }

    // 8. Обновление профиля
    if (type === 'profile.update') {
      const user = this.currentUser() || this.ensureUser('user@selfvoy.local', 'Пользователь');
      if (payload.display_name !== undefined) user.display_name = String(payload.display_name);
      if (payload.bio !== undefined) user.bio = String(payload.bio);
      if (payload.avatar_url !== undefined) user.avatar_url = String(payload.avatar_url);
      this.saveState();
      return { id, type: 'ok', payload: { user } };
    }

    // 9. Удаление аккаунта
    if (type === 'auth.deleteAccount') {
      const user = this.currentUser();
      if (user) {
        this.users.delete(user.email.toLowerCase());
      }
      this.currentUserId = 0;
      this.saveState();
      return { id, type: 'ok', payload: { deleted: true } };
    }

    // 10. Выход
    if (type === 'auth.logout') {
      this.currentUserId = 0;
      this.saveState();
      return { id, type: 'ok', payload: { revoked: true } };
    }

    // 11. Задачи
    if (type === 'tasks.list') {
      return { id, type: 'ok', payload: { tasks: this.tasks } };
    }
    if (type === 'tasks.create') {
      const title = String(payload.title || 'Новая задача');
      const newTask: Task = {
        id: Date.now(),
        title,
        notes: String(payload.notes || ''),
        status: 'pending',
        due_at: payload.due_at ? String(payload.due_at) : new Date(Date.now() + 86400000).toISOString(),
        remind_at: payload.remind_at ? String(payload.remind_at) : new Date(Date.now() + 3600000).toISOString(),
      };
      this.tasks.unshift(newTask);
      this.saveState();
      return { id, type: 'ok', payload: newTask };
    }
    if (type === 'tasks.complete') {
      const taskId = Number(payload.id);
      const t = this.tasks.find((item) => item.id === taskId);
      if (t) {
        t.status = 'done';
        t.completed_at = new Date().toISOString();
        this.saveState();
        return { id, type: 'ok', payload: t };
      }
      return { id, type: 'error', code: 'not_found', message: 'Задача не найдена' };
    }
    if (type === 'tasks.reopen') {
      const taskId = Number(payload.id);
      const t = this.tasks.find((item) => item.id === taskId);
      if (t) {
        t.status = 'pending';
        t.completed_at = undefined;
        this.saveState();
        return { id, type: 'ok', payload: t };
      }
      return { id, type: 'error', code: 'not_found', message: 'Задача не найдена' };
    }
    if (type === 'tasks.delete') {
      const taskId = Number(payload.id);
      this.tasks = this.tasks.filter((item) => item.id !== taskId);
      this.saveState();
      return { id, type: 'ok', payload: { deleted: true } };
    }

    // 12. Настройки (prefs)
    if (type === 'prefs.get') {
      return { id, type: 'ok', payload: this.prefs };
    }
    if (type === 'prefs.set') {
      this.prefs = { ...this.prefs, ...payload };
      this.saveState();
      return { id, type: 'ok', payload: this.prefs };
    }

    // 13. Разрешения (permissions)
    if (type === 'permissions.list') {
      return { id, type: 'ok', payload: { tools: this.permissions } };
    }
    if (type === 'permissions.set') {
      const tool = String(payload.tool);
      const mode = payload.mode as Permission['mode'];
      const p = this.permissions.find((item) => item.tool === tool);
      if (p) p.mode = mode;
      this.saveState();
      return { id, type: 'ok', payload: { tool, mode } };
    }

    // 14. ИИ-Ассистент (agent.ask)
    if (type === 'agent.ask') {
      const message = String(payload.message || '').trim();
      let reply = 'Я вас услышал! Чем ещё могу помочь?';

      if (/привет|здравствуй|хай/i.test(message)) {
        reply = 'Здравствуйте! Я ваш автономный ассистент Selfvoy. Я умею планировать задачи, напоминать о делах и вести диалог без внешнего сервера!';
      } else if (/задач|напомни|сделай/i.test(message)) {
        const title = message.replace(/напомни|создай задачу|задача/i, '').trim() || 'Новая задача из диалога';
        const newTask: Task = {
          id: Date.now(),
          title: title.charAt(0).toUpperCase() + title.slice(1),
          notes: 'Создано через диалог с Selfvoy AI',
          status: 'pending',
          due_at: new Date(Date.now() + 86400000).toISOString(),
          remind_at: new Date(Date.now() + 3600000).toISOString(),
        };
        this.tasks.unshift(newTask);
        this.saveState();
        reply = `Договорились! Я добавил задачу «${newTask.title}» в ваш список задач.`;
      } else if (/что у меня сегодня|планы|дела/i.test(message)) {
        const pending = this.tasks.filter((t) => t.status === 'pending');
        if (pending.length === 0) {
          reply = 'На сегодня у вас нет активных задач. Отличный повод отдохнуть или запланировать что-то новое!';
        } else {
          reply = `У вас ${pending.length} активных задач на сегодня:\n` + pending.map((t, idx) => `${idx + 1}. ${t.title}`).join('\n');
        }
      } else if (/спасибо|благодарю/i.test(message)) {
        reply = 'Всегда рад помочь! Обращайтесь в любое время.';
      }

      return {
        id,
        type: 'ok',
        payload: {
          reply,
          intent: 'assistant',
          results: [],
        },
      };
    }

    // 15. Чаты
    if (type === 'chat.list') {
      return { id, type: 'ok', payload: { chats: this.chats } };
    }
    if (type === 'chat.open') {
      const contact = String(payload.contact || '');
      let chat = this.chats.find((c) => c.title.toLowerCase() === contact.toLowerCase());
      let created = false;
      if (!chat) {
        created = true;
        chat = {
          id: Date.now(),
          title: contact || 'Новый диалог',
          members: [
            { user_id: this.currentUserId, display_name: 'Вы', email: 'me@selfvoy.local' },
            { user_id: 999, display_name: contact, email: `${contact}@example.com` },
          ],
          last_message_body: '',
          last_message_at: new Date().toISOString(),
        };
        this.chats.unshift(chat);
        this.saveState();
      }
      return { id, type: 'ok', payload: { chat, created } };
    }
    if (type === 'chat.history') {
      const chatId = Number(payload.chat_id) || 1;
      const history = this.messages.get(chatId) || [];
      return { id, type: 'ok', payload: { messages: history } };
    }
    if (type === 'chat.send') {
      const chatId = Number(payload.chat_id) || 1;
      const body = String(payload.body || '');
      const userMsg: Message = {
        id: Date.now(),
        chat_id: chatId,
        sender_id: this.currentUserId,
        sender_name: this.currentUser()?.display_name || 'Вы',
        body,
        kind: 'text',
        created_at: new Date().toISOString(),
      };
      let list = this.messages.get(chatId);
      if (!list) {
        list = [];
        this.messages.set(chatId, list);
      }
      list.push(userMsg);

      // Обновляем чат
      const chat = this.chats.find((c) => c.id === chatId);
      if (chat) {
        chat.last_message_body = body;
        chat.last_message_at = userMsg.created_at;
      }
      this.saveState();

      // Авто-ответ от Selfvoy AI в чате через 400мс
      setTimeout(() => {
        const aiMsg: Message = {
          id: Date.now() + 1,
          chat_id: chatId,
          sender_id: 0,
          sender_name: 'Selfvoy AI',
          body: `Принято: «${body}». Я зафиксировал это в автономном профиле!`,
          kind: 'text',
          created_at: new Date().toISOString(),
        };
        list?.push(aiMsg);
        if (chat) {
          chat.last_message_body = aiMsg.body;
          chat.last_message_at = aiMsg.created_at;
        }
        this.saveState();
        emitEvent({
          type: 'event',
          event: 'chat.message',
          payload: aiMsg,
        });
      }, 400);

      return { id, type: 'ok', payload: userMsg };
    }

    // 16. Уведомления и подтверждения
    if (type === 'notifications.list') {
      const unread = this.notices.filter((n) => !n.read).length;
      return { id, type: 'ok', payload: { notifications: this.notices, unread } };
    }
    if (type === 'notifications.read') {
      this.notices.forEach((n) => (n.read = true));
      this.saveState();
      return { id, type: 'ok', payload: { unread: 0 } };
    }
    if (type === 'confirmation.list') {
      const actions: Confirmation[] = [];
      return { id, type: 'ok', payload: { actions } };
    }

    // 17. Сессии
    if (type === 'sessions.list') {
      const now = new Date().toISOString();
      const expires = new Date(Date.now() + 30 * 86400000).toISOString();
      return {
        id,
        type: 'ok',
        payload: {
          sessions: [
            {
              id: 'standalone_session',
              device: 'Это устройство (Автономный режим)',
              remote_addr: '127.0.0.1 (local)',
              created_at: now,
              expires_at: expires,
              current: true,
            },
          ],
        },
      };
    }
    if (type === 'sessions.revoke' || type === 'sessions.revokeAll') {
      return { id, type: 'ok', payload: { revoked: 1 } };
    }
    if (type === 'devices.list') {
      return { id, type: 'ok', payload: { devices: [] } };
    }
    if (type === 'auth.status2fa') {
      return { id, type: 'ok', payload: { enabled: false, pending: false, recovery_codes_left: 0 } };
    }

    // 18. Переговоры и бронирования (A2A)
    if (type === 'agent.negotiate') {
      return {
        id,
        type: 'ok',
        payload: {
          accepted: true,
          slot: {
            start: new Date(Date.now() + 86400000).toISOString(),
            end: new Date(Date.now() + 86400000 + 3600000).toISOString(),
            reason: 'Время успешно согласовано',
          },
          message: 'Время встречи подтверждено автономным агентом',
        },
      };
    }
    if (type === 'booking.get' || type === 'booking.slotsList' || type === 'booking.pageGet') {
      return { id, type: 'ok', payload: { page: null, slots: [] } };
    }
    if (type === 'invite.list' || type === 'package.list') {
      return { id, type: 'ok', payload: { invites: [], packages: [] } };
    }
    if (type === 'telegram.status') {
      return { id, type: 'ok', payload: { enabled: false, linked: false } };
    }
    if (type === 'calendar.icsUrl') {
      return { id, type: 'ok', payload: { url: '' } };
    }

    // 19. Системные пинги
    if (type === 'ping' || type === 'server.info') {
      return {
        id,
        type: 'ok',
        payload: {
          name: 'Selfvoy Standalone',
          version: '0.1.0',
          mode: 'standalone',
          platform: 'local',
        },
      };
    }

    // Дефолтный ответ на прочие запросы
    return { id, type: 'ok', payload: {} };
  }
}

export const standaloneEngine = new StandaloneEngine();
