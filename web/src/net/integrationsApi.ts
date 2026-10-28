/*
  Подключения владельца (нужен вход), docs/PROTOCOL.md:
  - «Telegram-бот» (F2): telegram.status / link / unlink;
  - «Цена, предоплата и политика отмены» (F4): payments.status / connect / disconnect —
    свой магазин ЮKassa, деньги гостей идут владельцу напрямую;
  - «Календарь: ICS-подписка» (F6): calendar.feed / feedReset / feedDisable.

  Секретный ключ ЮKassa уходит на сервер один раз в payments.connect и нигде на
  устройстве не сохраняется и не пишется в журнал.
*/
import type { SelfvoySocket } from './socket';

export interface TelegramStatus {
  /** Бот настроен на сервере. */
  enabled: boolean;
  bot_username: string;
  /** К аккаунту привязан чат. */
  linked: boolean;
  /** Ник в Telegram или "". */
  username: string;
}

export interface TelegramLink {
  /** https://t.me/<бот>?start=o_… — живёт 15 минут. */
  url: string;
  expires_at: string;
}

export interface PaymentsStatus {
  connected: boolean;
  provider: 'yookassa' | '';
  shop_id: string;
  connected_at: string;
}

/**
  Подписка на календарь: url/path видны только в ответе calendar.feedReset (токен
  в БД не хранится); calendar.feed отдаёт их пустыми.
*/
export interface CalendarFeed {
  enabled: boolean;
  url: string;
  path: string;
}

export function makeIntegrationsApi(socket: SelfvoySocket) {
  const call = <T,>(type: string, payload: Record<string, unknown> = {}) => socket.request<T>(type, payload);
  return {
    // -------- Telegram
    telegramStatus: () => call<TelegramStatus>('telegram.status'),
    telegramLink: () => call<TelegramLink>('telegram.link'),
    telegramUnlink: () => call<{ linked: false }>('telegram.unlink'),

    // -------- ЮKassa
    paymentsStatus: () => call<PaymentsStatus>('payments.status'),
    paymentsConnect: (shop_id: string, secret_key: string) =>
      call<PaymentsStatus>('payments.connect', { provider: 'yookassa', shop_id, secret_key }),
    paymentsDisconnect: () => call<{ connected: false }>('payments.disconnect'),

    // -------- календарь (ICS)
    calendarFeed: () => call<CalendarFeed>('calendar.feed'),
    calendarFeedReset: () => call<CalendarFeed>('calendar.feedReset'),
    calendarFeedDisable: () => call<{ enabled: false }>('calendar.feedDisable'),
  };
}

export type IntegrationsApi = ReturnType<typeof makeIntegrationsApi>;
