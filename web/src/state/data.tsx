/*
  Данные приложения с сервера Selfvoy: настройки, задачи, уведомления,
  подтверждения, разрешения, чаты/контакты, диалог с Selfvoy и переговоры.

  Загружаются после входа и при каждом переподключении; push-события сервера
  (chat.message, notification.new, task.due) обновляют нужные списки сразу.
  Диалог с Selfvoy и история переговоров — локальная лента этого устройства
  (сервер хранит их результат: задачи, сообщения в чатах, подтверждения).
*/
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import type { Chat, Confirmation, NegotiationResult, Notice, Permission, Prefs, Privacy, Task } from '../net/api';
import { pushService } from '../net/pushService';
import { isNetworkError } from '../net/socket';
import { dialogKey as dialogKeyOf, negotiationsKey as negotiationsKeyOf } from '../net/userCache';
import { errorText, useSession } from './session';

export interface DialogAction {
  confirmationId: number;
  summary: string;
  tool: string;
}

export interface DialogEntry {
  id: string;
  from: 'user' | 'selfvoy';
  text: string;
  at: string;
  error?: boolean;
  actions?: DialogAction[];
  negotiationId?: string;
}

export interface NegotiationRecord {
  id: string;
  at: string;
  target: string;
  targetName: string;
  topic: string;
  durationMinutes: number;
  windowHours: number;
  result: NegotiationResult;
  booked?: boolean;
}

export interface Contact {
  userId: number;
  name: string;
  email: string;
  avatar: string;
  chatId: number;
  lastBody: string;
  lastAt: string;
}

export interface Toast {
  id: number;
  text: string;
  kind: 'info' | 'error';
}

interface Data {
  prefs: Prefs | null;
  tasks: Task[];
  notices: Notice[];
  unread: number;
  confirmations: Confirmation[];
  permissions: Permission[];
  chats: Chat[];
  contacts: Contact[];
  dialog: DialogEntry[];
  negotiations: NegotiationRecord[];
  thinking: boolean;
  loaded: boolean;
  toast: Toast | null;
  /** Последнее сообщение из чата (событие chat.message) — для открытой переписки. */
  chatPulse: number;

  reload(): Promise<void>;
  /** Поля или функция от актуальных настроек (для переключателей: без устаревших замыканий). */
  savePrefs(fields: Partial<Prefs> | ((current: Prefs) => Partial<Prefs>)): Promise<boolean>;
  savePrivacy(fields: Partial<Privacy>): Promise<boolean>;
  setPermission(tool: string, mode: Permission['mode']): Promise<void>;
  addTask(title: string, due?: Date): Promise<boolean>;
  toggleTask(task: Task): Promise<void>;
  deleteTask(id: number): Promise<void>;
  readAll(): Promise<void>;
  approve(id: number): Promise<void>;
  deny(id: number): Promise<void>;
  ask(text: string): Promise<string | null>;
  negotiate(target: string, targetName: string, topic: string, durationMinutes: number, windowHours: number): Promise<string | null>;
  book(id: string): Promise<void>;
  openChat(contact: string): Promise<number | null>;
  notify(text: string, kind?: Toast['kind']): void;
}

const Ctx = createContext<Data | null>(null);

let seq = 0;
const uid = (p: string) => `${p}-${Date.now().toString(36)}-${(seq++).toString(36)}`;

function readLocal<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

function writeLocal(key: string, value: unknown) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    /* ленту на устройстве не сохранить — живёт до перезапуска */
  }
}

export function DataProvider({ children }: { children: ReactNode }) {
  const { api, socket, conn, status, user } = useSession();
  const [prefs, setPrefsView] = useState<Prefs | null>(null);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [notices, setNotices] = useState<Notice[]>([]);
  const [unread, setUnread] = useState(0);
  const [confirmations, setConfirmations] = useState<Confirmation[]>([]);
  const [permissions, setPermissionsView] = useState<Permission[]>([]);
  const [chats, setChats] = useState<Chat[]>([]);
  const [dialog, setDialog] = useState<DialogEntry[]>([]);
  const [negotiations, setNegotiations] = useState<NegotiationRecord[]>([]);
  const [thinking, setThinking] = useState(false);
  const [loaded, setLoaded] = useState(false);
  const [toast, setToast] = useState<Toast | null>(null);
  const [chatPulse, setChatPulse] = useState(0);
  const toastTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);

  // Настройки и разрешения меняются оптимистично и из нескольких мест подряд:
  // актуальное значение держим в ref, чтобы откат и следующий клик видели
  // свежие данные, а не снимок из замыкания прошлого рендера.
  const prefsRef = useRef<Prefs | null>(null);
  const prefsInflight = useRef(0);
  const setPrefsState = useCallback((next: Prefs | null) => {
    prefsRef.current = next;
    setPrefsView(next);
  }, []);
  const permissionsRef = useRef<Permission[]>([]);
  const setPermissions = useCallback((next: Permission[]) => {
    permissionsRef.current = next;
    setPermissionsView(next);
  }, []);

  const userId = user?.id ?? 0;
  const dialogKey = dialogKeyOf(userId);
  const negotiationsKey = negotiationsKeyOf(userId);

  const notify = useCallback((text: string, kind: Toast['kind'] = 'info') => {
    clearTimeout(toastTimer.current);
    setToast({ id: Date.now(), text, kind });
    toastTimer.current = setTimeout(() => setToast(null), 4200);
  }, []);

  /** Ошибка запроса: сетевые не шумим (есть баннер «нет связи»), остальные — тостом. */
  const fail = useCallback(
    (error: unknown) => {
      if (!isNetworkError(error)) notify(errorText(error), 'error');
    },
    [notify],
  );

  // Локальная лента — своя у каждого пользователя.
  useEffect(() => {
    if (!userId) {
      setDialog([]);
      setNegotiations([]);
      return;
    }
    setDialog(readLocal<DialogEntry[]>(dialogKey, []));
    setNegotiations(readLocal<NegotiationRecord[]>(negotiationsKey, []));
  }, [userId, dialogKey, negotiationsKey]);
  useEffect(() => {
    if (userId) writeLocal(dialogKey, dialog.slice(-120));
  }, [dialog, dialogKey, userId]);
  useEffect(() => {
    if (userId) writeLocal(negotiationsKey, negotiations.slice(0, 40));
  }, [negotiations, negotiationsKey, userId]);

  const loadTasks = useCallback(() => api.tasks().then((r) => setTasks(r.tasks ?? [])), [api]);
  const loadNotices = useCallback(
    () =>
      api.notifications().then((r) => {
        setNotices(r.notifications ?? []);
        setUnread(Number(r.unread ?? 0));
      }),
    [api],
  );
  const loadConfirmations = useCallback(() => api.confirmations().then((r) => setConfirmations(r.actions ?? [])), [api]);
  const loadChats = useCallback(() => api.chats().then((r) => setChats(r.chats ?? [])), [api]);

  const reload = useCallback(async () => {
    const results = await Promise.allSettled([
      api.prefs().then(setPrefsState),
      loadTasks(),
      loadNotices(),
      loadConfirmations(),
      api.permissions().then((r) => setPermissions(r.tools ?? [])),
      loadChats(),
    ]);
    const firstError = results.find((r) => r.status === 'rejected') as PromiseRejectedResult | undefined;
    if (firstError) fail(firstError.reason);
    setLoaded(true);
  }, [api, loadTasks, loadNotices, loadConfirmations, loadChats, fail, setPrefsState, setPermissions]);

  // Вход и каждое переподключение — свежие данные.
  useEffect(() => {
    if (status === 'signedIn' && conn === 'open') reload();
    if (status === 'signedOut') {
      setLoaded(false);
      setPrefsState(null);
      setTasks([]);
      setNotices([]);
      setConfirmations([]);
      setPermissions([]);
      setChats([]);
    }
  }, [status, conn, reload, setPrefsState, setPermissions]);

  // Живые события сервера.
  useEffect(
    () =>
      socket.onEvent((event) => {
        switch (event.event) {
          case 'chat.message':
          case 'chat.created':
            loadChats().catch(() => undefined);
            setChatPulse((n) => n + 1);
            break;
          case 'task.due':
            loadTasks().catch(() => undefined);
            loadNotices().catch(() => undefined);
            break;
          case 'notification.new': {
            loadNotices().catch(() => undefined);
            const notice = event.payload as { kind?: string; title?: unknown; body?: unknown };
            if (notice.kind === 'confirmation.requested') loadConfirmations().catch(() => undefined);
            // Абонементы (F5): покупка, оплата, «осталось мало занятий» — сразу всплывающей строкой.
            if (notice.kind?.startsWith('package.') && typeof notice.title === 'string' && notice.title) notify(notice.title);
            // Системное push-уведомление на телефоне/ОС (встречи, бронирования, напоминания)
            if (typeof notice.title === 'string' && notice.title) {
              const bodyStr = typeof notice.body === 'string' ? notice.body : '';
              pushService.showLocalNotification(notice.title, bodyStr);
            }
            break;
          }
        }
      }),
    [socket, loadChats, loadTasks, loadNotices, loadConfirmations, notify],
  );

  const contacts = useMemo<Contact[]>(() => {
    const out: Contact[] = [];
    for (const chat of chats) {
      const others = (chat.members ?? []).filter((m) => m.user_id !== userId);
      if (others.length !== 1) continue;
      const peer = others[0];
      if (out.some((c) => c.userId === peer.user_id)) continue;
      out.push({
        userId: peer.user_id,
        name: peer.display_name || peer.email,
        email: peer.email,
        avatar: peer.avatar_url ?? '',
        chatId: chat.id,
        lastBody: chat.last_message_body ?? '',
        lastAt: chat.last_message_at ?? '',
      });
    }
    return out;
  }, [chats, userId]);

  /**
   * Оптимистичная правка настроек: экран меняется сразу. Ответ сервера применяется,
   * только когда других правок в пути нет (иначе он «откатил» бы более свежий клик);
   * при ошибке возвращаются лишь затронутые поля.
   */
  const commitPrefs = useCallback(
    async (apply: (p: Prefs) => Prefs, body: Partial<Prefs> | { privacy: Partial<Privacy> }, revert: (now: Prefs) => Prefs) => {
      const current = prefsRef.current;
      if (current) setPrefsState(apply(current));
      prefsInflight.current += 1;
      try {
        const saved = await api.setPrefs(body);
        if (prefsInflight.current === 1) setPrefsState(saved);
        return true;
      } catch (error) {
        const now = prefsRef.current;
        if (now) setPrefsState(revert(now));
        fail(error);
        return false;
      } finally {
        prefsInflight.current -= 1;
      }
    },
    [api, fail, setPrefsState],
  );

  const savePrefs = useCallback(
    async (input: Partial<Prefs> | ((current: Prefs) => Partial<Prefs>)) => {
      const current = prefsRef.current;
      if (typeof input === 'function' && !current) return false;
      const fields = typeof input === 'function' ? input(current as Prefs) : input;
      const keys = Object.keys(fields) as (keyof Prefs)[];
      const previous: Partial<Prefs> = {};
      if (current) for (const key of keys) (previous as Record<string, unknown>)[key] = current[key];
      return commitPrefs(
        (p) => ({ ...p, ...fields }),
        fields,
        (now) => ({ ...now, ...previous }),
      );
    },
    [commitPrefs],
  );

  const savePrivacy = useCallback(
    async (fields: Partial<Privacy>) => {
      const current = prefsRef.current;
      const previous: Partial<Privacy> = {};
      if (current) for (const key of Object.keys(fields) as (keyof Privacy)[]) previous[key] = current.privacy?.[key];
      return commitPrefs(
        (p) => ({ ...p, privacy: { ...p.privacy, ...fields } }),
        { privacy: fields },
        (now) => ({ ...now, privacy: { ...now.privacy, ...previous } }),
      );
    },
    [commitPrefs],
  );

  const setPermission = useCallback(
    async (tool: string, mode: Permission['mode']) => {
      const before = permissionsRef.current.find((p) => p.tool === tool)?.mode;
      setPermissions(permissionsRef.current.map((p) => (p.tool === tool ? { ...p, mode } : p)));
      try {
        await api.setPermission(tool, mode);
      } catch (error) {
        // Откатываем только этот инструмент и только если после нас его не меняли.
        if (before) setPermissions(permissionsRef.current.map((p) => (p.tool === tool && p.mode === mode ? { ...p, mode: before } : p)));
        fail(error);
      }
    },
    [api, fail, setPermissions],
  );

  const addTask = useCallback(
    async (title: string, due?: Date) => {
      try {
        const iso = due?.toISOString();
        await api.createTask(title.trim(), iso, iso);
        await loadTasks();
        return true;
      } catch (error) {
        fail(error);
        return false;
      }
    },
    [api, loadTasks, fail],
  );

  const toggleTask = useCallback(
    async (task: Task) => {
      const done = task.status === 'done';
      setTasks((list) => list.map((t) => (t.id === task.id ? { ...t, status: done ? 'pending' : 'done' } : t)));
      try {
        await (done ? api.reopenTask(task.id) : api.completeTask(task.id));
      } catch (error) {
        fail(error);
      }
      loadTasks().catch(() => undefined);
    },
    [api, loadTasks, fail],
  );

  const deleteTask = useCallback(
    async (id: number) => {
      setTasks((list) => list.filter((t) => t.id !== id));
      try {
        await api.deleteTask(id);
      } catch (error) {
        fail(error);
        loadTasks().catch(() => undefined);
      }
    },
    [api, loadTasks, fail],
  );

  const readAll = useCallback(async () => {
    try {
      const r = await api.readNotifications(0);
      setUnread(Number(r.unread ?? 0));
      setNotices((list) => list.map((n) => ({ ...n, read: true })));
    } catch (error) {
      fail(error);
    }
  }, [api, fail]);

  const settle = useCallback(
    async (id: number, approve: boolean) => {
      try {
        const r = await (approve ? api.approve(id) : api.deny(id));
        notify(approve ? (r.status === 'executed' ? 'Готово — Selfvoy выполнил действие' : 'Не получилось выполнить действие') : 'Отклонено');
      } catch (error) {
        fail(error);
      }
      await Promise.allSettled([loadConfirmations(), loadNotices(), loadChats()]);
    },
    [api, notify, fail, loadConfirmations, loadNotices, loadChats],
  );

  const pushDialog = useCallback((entry: Omit<DialogEntry, 'id' | 'at'>) => {
    const full = { ...entry, id: uid('m'), at: new Date().toISOString() };
    setDialog((list) => [...list, full]);
    return full.id;
  }, []);

  const recordNegotiation = useCallback(
    (target: string, targetName: string, topic: string, durationMinutes: number, windowHours: number, result: NegotiationResult) => {
      const record: NegotiationRecord = { id: uid('n'), at: new Date().toISOString(), target, targetName, topic, durationMinutes, windowHours, result };
      setNegotiations((list) => [record, ...list]);
      return record.id;
    },
    [],
  );

  const ask = useCallback(
    async (text: string) => {
      const message = text.trim();
      if (!message) return null;
      pushDialog({ from: 'user', text: message });
      setThinking(true);
      try {
        const r = await api.ask(message);
        const actions = (r.results ?? [])
          .filter((a) => a.requires_confirmation && a.confirmation_id)
          .map((a) => ({ confirmationId: Number(a.confirmation_id), summary: a.summary ?? a.tool, tool: a.tool }));
        let negotiationId: string | undefined;
        if (r.a2a && typeof r.a2a.accepted === 'boolean' && !r.a2a_note) {
          negotiationId = recordNegotiation(r.a2a.target ?? '', r.a2a.target ?? '', '', 60, 96, r.a2a);
        }
        pushDialog({ from: 'selfvoy', text: r.reply || 'Готово', actions: actions.length ? actions : undefined, negotiationId });
        if (actions.length) loadConfirmations().catch(() => undefined);
        loadTasks().catch(() => undefined);
        return negotiationId ?? null;
      } catch (error) {
        pushDialog({ from: 'selfvoy', text: errorText(error), error: true });
        return null;
      } finally {
        setThinking(false);
      }
    },
    [api, pushDialog, recordNegotiation, loadConfirmations, loadTasks],
  );

  const negotiate = useCallback(
    async (target: string, targetName: string, topic: string, durationMinutes: number, windowHours: number) => {
      try {
        const result = await api.negotiate(target, topic, durationMinutes, windowHours);
        return recordNegotiation(target, targetName, topic, durationMinutes, windowHours, result);
      } catch (error) {
        fail(error);
        return null;
      }
    },
    [api, recordNegotiation, fail],
  );

  const book = useCallback(
    async (id: string) => {
      const record = negotiations.find((n) => n.id === id);
      const slot = record?.result.slot;
      if (!record || !slot) return;
      if (permissionsRef.current.find((p) => p.tool === 'book_table')?.mode === 'deny') {
        notify('Бронирование запрещено в разрешениях Selfvoy', 'error');
        return;
      }
      try {
        const place = record.result.place?.name;
        if (place) await api.book(place, slot.start);
        const remind = new Date(new Date(slot.start).getTime() - 30 * 60_000).toISOString();
        await api.createTask(`Встреча с ${record.targetName}${place ? ` — ${place}` : ''}`, slot.start, remind);
        setNegotiations((list) => list.map((n) => (n.id === id ? { ...n, booked: true } : n)));
        await loadTasks();
        notify(place ? `Столик в «${place}» забронирован, встреча в задачах` : 'Встреча добавлена в задачи');
      } catch (error) {
        fail(error);
      }
    },
    [api, negotiations, notify, fail, loadTasks],
  );

  const openChat = useCallback(
    async (contact: string) => {
      try {
        const r = await api.openChat(contact);
        await loadChats();
        return r.chat.id;
      } catch (error) {
        fail(error);
        return null;
      }
    },
    [api, loadChats, fail],
  );

  const value: Data = {
    prefs,
    tasks,
    notices,
    unread,
    confirmations,
    permissions,
    chats,
    contacts,
    dialog,
    negotiations,
    thinking,
    loaded,
    toast,
    chatPulse,
    reload,
    savePrefs,
    savePrivacy,
    setPermission,
    addTask,
    toggleTask,
    deleteTask,
    readAll,
    approve: (id) => settle(id, true),
    deny: (id) => settle(id, false),
    ask,
    negotiate,
    book,
    openChat,
    notify,
  };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useData(): Data {
  const value = useContext(Ctx);
  if (!value) throw new Error('useData outside DataProvider');
  return value;
}
