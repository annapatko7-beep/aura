/*
  Приглашение по ссылке (docs/PROTOCOL.md → «Приглашения», invite.*).

  Владелец (нужен вход): invite.create / invite.list / invite.cancel.
  Гость (без входа, токен в payload): invite.get / invite.respond — работают и на
  неавторизованном соединении, и на соединении вошедшего пользователя.

  Токен приглашения сервер отдаёт один раз — в ответе invite.create; в БД хранится
  только его хэш, в invite.list токена нет.
*/
import type { SelfvoySocket } from './socket';

export type InviteStatus = 'open' | 'accepted' | 'declined' | 'cancelled' | 'expired';

export interface InviteSlot {
  start: string;
  end: string;
}

/** Приглашение глазами владельца. `chosen` — `{}`, пока время не выбрано. */
export interface Invite {
  id: number;
  topic: string;
  note: string;
  duration_minutes: number;
  status: InviteStatus;
  slots: InviteSlot[];
  guest_name: string;
  guest_email: string;
  guest_comment: string;
  chosen: InviteSlot | Record<string, never>;
  expires_at: string;
  created_at: string;
  responded_at: string;
}

/** Приглашение глазами гостя: без email владельца, слоты — только будущие и свободные. */
export interface GuestInvite {
  topic: string;
  note: string;
  duration_minutes: number;
  owner_name: string;
  owner_timezone: string;
  status: InviteStatus;
  slots: InviteSlot[];
  guest_name: string;
  chosen: InviteSlot | Record<string, never>;
  expires_at: string;
  /** Реферальный код владельца (старые серверы не присылают). */
  ref?: string;
}

export interface CreateInvite {
  topic: string;
  duration_minutes: number;
  window_hours: number;
  note?: string;
  guest_name?: string;
}

export interface CreatedInvite {
  invite: Invite;
  /** 43 символа base64url; показывается только сейчас. */
  token: string;
  /** Готовая ссылка или "" — если на сервере не задан SELFVOY_PUBLIC_URL. */
  url: string;
}

export interface InviteAnswer {
  action: 'accept' | 'decline';
  /** Для accept — ровно start одного из предложенных слотов. */
  start?: string;
  guest_name: string;
  guest_email?: string;
  comment?: string;
}

/** Подбор слотов идёт через ИИ-сервис — ждём дольше обычного, как agent.negotiate. */
const CREATE_TIMEOUT_MS = 90_000;

export function makeInviteApi(socket: SelfvoySocket) {
  const call = <T,>(type: string, payload: Record<string, unknown> = {}, timeout?: number) =>
    timeout === undefined ? socket.request<T>(type, payload) : socket.request<T>(type, payload, timeout);
  return {
    // -------- владелец
    create: (params: CreateInvite) => {
      const payload: Record<string, unknown> = {
        topic: params.topic,
        duration_minutes: params.duration_minutes,
        window_hours: params.window_hours,
      };
      if (params.note) payload.note = params.note;
      if (params.guest_name) payload.guest_name = params.guest_name;
      return call<CreatedInvite>('invite.create', payload, CREATE_TIMEOUT_MS);
    },
    list: (limit = 50) => call<{ invites: Invite[] }>('invite.list', { limit }),
    cancel: (invite_id: number) => call<{ invite: Invite }>('invite.cancel', { invite_id }),

    // -------- гость (публичные методы)
    get: (token: string) => call<{ invite: GuestInvite }>('invite.get', { token }),
    respond: (token: string, answer: InviteAnswer) => {
      const payload: Record<string, unknown> = { token, action: answer.action, guest_name: answer.guest_name };
      if (answer.action === 'accept') payload.start = answer.start;
      if (answer.guest_email) payload.guest_email = answer.guest_email;
      if (answer.comment) payload.comment = answer.comment;
      return call<{ invite: GuestInvite }>('invite.respond', payload);
    },
  };
}

export type InviteApi = ReturnType<typeof makeInviteApi>;
