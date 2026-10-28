/*
  Типизированные вызовы протокола Selfvoy поверх SelfvoySocket. Имена методов и полей —
  ровно как в docs/PROTOCOL.md (те же, что у Qt/Android/iOS-клиентов).
*/
import type { SelfvoySocket } from './socket';

/**
  Тариф (docs/PROTOCOL.md → «Авторизация»): name честный (Pro, пока until в будущем)
  независимо от enforced; enforced=false — бета, функции Pro открыты всем.
*/
export interface Plan {
  name: 'pro' | 'free';
  /** ISO-время окончания Pro или "". */
  until: string;
  enforced: boolean;
}

export interface User {
  id: number;
  email: string;
  display_name: string;
  avatar_url: string;
  bio: string;
  timezone?: string;
  email_verified?: boolean;
  /** Роль в системе (auth.* / auth.me); нет поля — обычный пользователь. */
  role?: 'user' | 'moderator' | 'admin';
  /** Тариф; старый сервер не присылает. */
  plan?: Plan;
}

export interface Privacy {
  share_free_slots_only: boolean;
  hide_event_titles: boolean;
  allow_place_choice: boolean;
}

export interface Prefs {
  diet: string[];
  transport: string;
  budget_limit: number;
  city: string;
  preferred_hours: number[];
  work_hours: number[];
  birthday: string;
  allergies: string[];
  onboarded: boolean;
  privacy: Privacy;
}

export interface AuthPayload {
  token: string;
  refresh_token: string;
  user: User;
}

export interface SessionRow {
  id: string;
  device: string;
  remote_addr: string;
  created_at: string;
  expires_at: string;
  current: boolean;
}

export interface TrustedDevice {
  id: number;
  device: string;
  remote_addr: string;
  created_at: string;
  expires_at: string;
  last_used_at?: string;
}

export interface TwoFaSetup {
  secret: string;
  otpauth_uri: string;
  issuer: string;
  digits: number;
  period: number;
}

export interface OAuthProvider {
  provider: 'google' | 'github' | 'yandex' | 'vk' | 'discord' | string;
  name: string;
  icon: string;
  enabled: boolean;
}

export interface OAuthBeginResult {
  provider: string;
  authorize_url: string;
  state: string;
  expires_in: number;
}

export interface Task {
  id: number;
  title: string;
  notes: string;
  status: 'pending' | 'done' | 'cancelled';
  due_at: string;
  remind_at: string;
  completed_at?: string;
}

export interface ChatMember {
  user_id: number;
  display_name: string;
  email: string;
  avatar_url?: string;
}

export interface Chat {
  id: number;
  title: string;
  members: ChatMember[];
  last_message_body?: string;
  last_message_at?: string;
}

export interface Message {
  id: number;
  chat_id?: number;
  sender_id: number;
  sender_name?: string;
  body: string;
  kind: string;
  created_at: string;
}

export interface FoundUser {
  id: number;
  email: string;
  display_name: string;
  avatar_url?: string;
}

export interface Notice {
  id: number;
  kind: string;
  title: string;
  body: string;
  read: boolean;
  created_at: string;
  payload?: Record<string, unknown>;
}

export interface Confirmation {
  id: number;
  tool: string;
  summary: string;
  status: string;
  created_at?: string;
}

export interface Permission {
  tool: string;
  description: string;
  dangerous: boolean;
  default_mode: string;
  mode: 'allow' | 'ask' | 'deny';
}

export interface Slot {
  start: string;
  end: string;
  reason?: string;
}

export interface Place {
  name?: string;
  city?: string;
  rating?: number | string;
  tags?: string[];
}

export interface NegotiationResult {
  accepted: boolean;
  slot?: Slot | null;
  place?: Place;
  counter_slots?: Slot[];
  message?: string;
}

export interface ActionResult {
  tool: string;
  ok?: boolean;
  mode?: string;
  denied?: boolean;
  requires_confirmation?: boolean;
  confirmation_id?: number;
  summary?: string;
}

export interface AskResult {
  reply: string;
  intent?: string;
  results?: ActionResult[];
  a2a?: NegotiationResult & { target?: string };
  a2a_note?: string;
}

type Body = Record<string, unknown>;

/** Обёртка: каждый метод — один тип сообщения протокола. */
export function makeApi(socket: SelfvoySocket) {
  const call = <T,>(type: string, payload: Body = {}, timeout?: number) => socket.request<T>(type, payload, timeout);
  return {
    // -------- аккаунт (docs/PROTOCOL.md → «Авторизация»)
    /** ref — реферальный код пригласившего (referral.*), ref_source — откуда пришёл человек. */
    register: (display_name: string, email: string, password: string, ref?: { code: string; source: string }) =>
      call<{ requires_verification: boolean; user: User; email_code?: string }>(
        'auth.register',
        ref ? { display_name, email, password, ref: ref.code, ref_source: ref.source } : { display_name, email, password },
      ),
    verifyEmail: (email: string, code: string) => call<{ verified: boolean; user: User }>('auth.verifyEmail', { email, code }),
    resendCode: (email: string) => call<{ message: string; email_code?: string }>('auth.resendCode', { email }),
    login: (email: string, password: string, device: string, device_id: string) =>
      call<AuthPayload>('auth.login', { email, password, device, device_id }),
    login2fa: (email: string, password: string, code: string, trust_device: boolean, device: string, device_id: string) =>
      call<AuthPayload>('auth.login2fa', { email, password, code, trust_device, device, device_id }),
    forgotPassword: (email: string) => call<{ message: string; reset_code?: string }>('auth.forgotPassword', { email }),
    resetPassword: (email: string, code: string, new_password: string) =>
      call<{ reset: boolean }>('auth.resetPassword', { email, code, new_password }),
    me: () => call<User & { preferences?: Prefs }>('auth.me'),
    logout: (timeout?: number) => call<{ revoked: boolean }>('auth.logout', {}, timeout),
    /** token/refresh_token — новая пара для этого устройства (прежние отозваны сервером). */
    changePassword: (old_password: string, new_password: string) =>
      call<{ changed: boolean; token?: string; refresh_token?: string }>('auth.changePassword', { old_password, new_password }),
    updateProfile: (fields: Partial<Pick<User, 'display_name' | 'bio' | 'avatar_url'>>) => call<{ user: User }>('profile.update', fields),
    deleteAccount: () => call<{ deleted: boolean }>('auth.deleteAccount', {}),
    // -------- OAuth (Google, GitHub, Yandex, VK, Discord)
    oauthProviders: () => call<{ providers: OAuthProvider[] }>('auth.oauth.providers'),
    oauthBegin: (provider: string, redirect_uri: string) =>
      call<OAuthBeginResult>('auth.oauth.begin', { provider, redirect_uri }),
    oauthCallback: (provider: string, code: string, state: string, device: string, device_id: string) =>
      call<AuthPayload>('auth.oauth.callback', { provider, code, state, device, device_id }),

    // -------- сессии и 2FA
    sessions: () => call<{ sessions: SessionRow[] }>('sessions.list'),
    revokeSession: (session_id: string) => call<{ revoked: boolean }>('sessions.revoke', { session_id }),
    revokeOtherSessions: () => call<{ revoked: number }>('sessions.revokeAll'),
    twoFaStatus: () => call<{ enabled: boolean; pending: boolean; recovery_codes_left: number }>('auth.status2fa'),
    twoFaSetup: () => call<TwoFaSetup>('auth.setup2fa'),
    twoFaConfirm: (code: string) => call<{ recovery_codes: string[] }>('auth.confirm2fa', { code }),
    twoFaDisable: (password: string) => call<{ ok: boolean }>('auth.disable2fa', { password }),
    trustedDevices: () => call<{ devices: TrustedDevice[] }>('devices.list'),
    revokeTrustedDevice: (id: number) => call<{ ok: boolean }>('devices.revoke', { id }),

    // -------- настройки и разрешения
    prefs: () => call<Prefs>('prefs.get'),
    setPrefs: (fields: Partial<Prefs> | { privacy: Partial<Privacy> }) => call<Prefs>('prefs.set', fields as Body),
    permissions: () => call<{ tools: Permission[] }>('permissions.list'),
    setPermission: (tool: string, mode: Permission['mode']) => call<{ tool: string; mode: string }>('permissions.set', { tool, mode }),

    // -------- Selfvoy
    ask: (message: string) => call<AskResult>('agent.ask', { message }, 90_000),
    negotiate: (target: string, topic: string, duration_minutes: number, window_hours: number) =>
      call<NegotiationResult>('agent.negotiate', { target, topic, duration_minutes, window_hours }, 90_000),
    confirmations: () => call<{ actions: Confirmation[] }>('confirmation.list'),
    approve: (id: number) => call<{ id: number; status: string }>('confirmation.approve', { id }),
    deny: (id: number) => call<{ id: number; status: string }>('confirmation.deny', { id }),
    book: (place: string, at: string, people = 2) => call<{ confirmation?: string }>('tool.run', { tool: 'book_table', args: { place, at, people } }),

    // -------- задачи
    tasks: () => call<{ tasks: Task[] }>('tasks.list', { limit: 200 }),
    createTask: (title: string, due_at?: string, remind_at?: string) =>
      call<Task>('tasks.create', { title, ...(due_at ? { due_at } : {}), ...(remind_at ? { remind_at } : {}) }),
    completeTask: (id: number) => call<Task>('tasks.complete', { id }),
    reopenTask: (id: number) => call<Task>('tasks.reopen', { id }),
    deleteTask: (id: number) => call<{ deleted: boolean }>('tasks.delete', { id }),

    // -------- люди и чаты
    chats: () => call<{ chats: Chat[] }>('chat.list', { limit: 100 }),
    openChat: (contact: string) => call<{ chat: Chat; created: boolean }>('chat.open', { contact }),
    history: (chat_id: number) => call<{ messages: Message[] }>('chat.history', { chat_id, limit: 100 }),
    send: (chat_id: number, body: string) => call<Message>('chat.send', { chat_id, body }),
    searchUsers: (query: string) => call<{ users: FoundUser[] }>('users.search', { query, limit: 20 }),

    // -------- уведомления
    notifications: () => call<{ notifications: Notice[]; unread: number }>('notifications.list', { limit: 50 }),
    readNotifications: (id = 0) => call<{ unread: number }>('notifications.read', { id }),
    registerPushDevice: (platform: 'fcm' | 'apns' | 'webhook' | 'dev', token: string) =>
      call<{ id: number; platform: string }>('notifications.device.register', { platform, token }),
    listPushDevices: () =>
      call<{ devices: Array<{ id: number; platform: string; created_at: string; last_seen_at?: string }> }>('notifications.device.list'),
    revokePushDevice: (id: number) =>
      call<{ id: number; revoked: boolean }>('notifications.device.revoke', { id }),
  };
}

export type Api = ReturnType<typeof makeApi>;
