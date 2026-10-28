/*
  Страницы записи (docs/PROTOCOL.md → «Страницы записи», booking.*) и
  реферальная программа (referral.*).

  Владелец (нужен вход): booking.pageCreate / pageList / pageUpdate / pageDelete,
  booking.list, booking.ownerCancel, booking.paymentSet (F4), booking.slotsPost (F6),
  referral.get.
  Гость (без входа): booking.get (по slug страницы), booking.book, booking.manage /
  reschedule / cancel / confirm (F1) / paymentClaim (F4) — по токену ссылки
  управления: сервер отдаёт его один раз, в ответе booking.book; в БД только хэш.

  Пакеты и второй получатель (F5): предложения пакетов — поле страницы `packages`,
  гость покупает пакет при записи (package_id) или записывается по ссылке пакета
  (package_token); копия напоминаний — cc_email. Методы package.* — в packageApi.ts.
*/
import type { Plan } from './api';
import type { SelfvoySocket } from './socket';
import type { InviteSlot } from './inviteApi';
import type { GuestPackage, PackageOffer } from './packageApi';

export type BookingPageStatus = 'active' | 'paused' | 'archived';
export type BookingStatus = 'active' | 'cancelled';
export type OccurrenceStatus = 'active' | 'cancelled';
/** Кто отменил: system — оплата не поступила к сроку (F4). */
export type CancelledBy = '' | 'guest' | 'owner' | 'system';

// ------------------------------------------------ цена, оплата, политика (F4)

export type Currency = 'RUB' | 'USD' | 'EUR' | 'KZT' | 'BYN';
export type Prepay = 'none' | 'full' | 'deposit';
export type LateCancel = 'allow' | 'block';
export type PaymentStatus = 'pending' | 'claimed' | 'paid' | 'refunded' | 'waived' | 'expired';
/** Что владелец может отметить сам (booking.paymentSet). */
export type OwnerPaymentStatus = 'paid' | 'waived' | 'refunded';

/** Оплата записи (тот же формат у пакетов F5). Суммы — в минимальных единицах (копейках). */
export interface Payment {
  status: PaymentStatus;
  amount: number;
  currency: Currency | string;
  kind: 'full' | 'deposit' | 'package';
  method: 'link' | 'yookassa';
  /** Только у владельца. */
  provider_id?: string;
  /** Куда отправить гостя платить; "" — только реквизиты. */
  action_url: string;
  due_at: string;
  paid_at: string;
  refunded_at: string;
}

export interface Policy {
  cancel_hours: number;
  late_cancel: LateCancel;
  text: string;
}

/** Подпись «Запись через Selfvoy» (F3): url пуст без SELFVOY_PUBLIC_URL. */
export interface Branding {
  show: boolean;
  text: string;
  url: string;
}

/** Поля цены и политики отмены страницы (booking.pageCreate / pageUpdate). */
export interface PriceFields {
  price: number;
  currency: Currency;
  prepay: Prepay;
  deposit: number;
  payment_url: string;
  payment_note: string;
  pay_within_minutes: number;
  cancel_hours: number;
  late_cancel: LateCancel;
  policy_text: string;
}

/**
  Страница записи глазами владельца. `url` пуст, если на сервере не задан SELFVOY_PUBLIC_URL.
  Поля цены и политики (F4) необязательны: старый сервер их не присылает.
*/
export interface BookingPage extends Partial<PriceFields> {
  id: number;
  slug: string;
  url: string;
  title: string;
  description: string;
  duration_minutes: number;
  days_ahead: number;
  max_per_day: number;
  notice_hours: number;
  max_series: number;
  weekends: boolean;
  status: BookingPageStatus;
  /** Напоминать гостю за сутки и за 2 часа (F1, бесплатно). Старый сервер — нет поля. */
  guest_reminders?: boolean;
  /** Скрыть подпись «Запись через Selfvoy» (F3, Pro). */
  hide_branding?: boolean;
  /** Предложения пакетов занятий (F5, до 5, Pro). Старый сервер — нет поля. */
  packages?: PackageOffer[];
  /** Предстоящих встреч по этой странице. */
  upcoming: number;
  created_at: string;
  updated_at: string;
}

export interface Occurrence extends InviteSlot {
  status: OccurrenceStatus;
  /** Местное время владельца «YYYY-MM-DDTHH:MM» (старые записи — пусто). */
  local?: string;
  /** Когда гость нажал «Приду» (F1); "" — не подтверждал. */
  confirmed_at?: string;
}

/** Напоминание гостю (F1): sent_at "" — ещё не отправлено. */
export interface Reminder {
  start: string;
  kind: '24h' | '2h';
  due_at: string;
  sent_at: string;
}

/** Запись гостя глазами владельца. */
export interface Booking {
  id: number;
  page_id: number;
  page_title: string;
  guest_name: string;
  guest_email: string;
  guest_comment: string;
  /** Второй получатель извещений (F5): "" — нет. Владелец видит адрес целиком. */
  cc_email?: string;
  /** Пакет, по которому сделана запись (F5): 0 — без пакета. */
  package_id?: number;
  /**
    Цена одной встречи на момент записи (мин. единицы currency): 0 — по абонементу или
    бесплатно. Смена цены страницы её не меняет. Старый сервер не присылает — тогда
    берём цену страницы (bookingMoney в lib/booking.ts).
  */
  price?: number;
  currency?: Currency | string;
  status: BookingStatus;
  cancelled_by: CancelledBy;
  reschedules: number;
  occurrences: Occurrence[];
  reminders?: Reminder[];
  /** Оплата целиком (null — без оплаты). */
  payment?: Payment | null;
  created_at: string;
  updated_at: string;
}

export interface GuestSlot extends InviteSlot {
  /** Местная дата владельца (YYYY-MM-DD). */
  day?: string;
}

/** Страница записи глазами гостя: без email владельца и чужих записей. */
export interface GuestPage {
  slug: string;
  title: string;
  description: string;
  duration_minutes: number;
  max_series: number;
  owner_name: string;
  owner_timezone: string;
  status: 'active' | 'paused';
  slots: GuestSlot[];
  /** Сервер не смог подобрать время (AI-сервис недоступен) — показать «попробуйте позже». */
  slots_unavailable: boolean;
  /** Реферальный код владельца — для ссылки «Заведите свой Selfvoy». */
  ref: string;
  branding?: Branding;
  price?: number;
  currency?: Currency | string;
  prepay?: Prepay;
  deposit?: number;
  payment_note?: string;
  payment_url?: string;
  pay_within_minutes?: number;
  policy?: Policy;
  /** Предложения пакетов (F5): гость может купить пакет при записи. */
  packages?: PackageOffer[];
}

/** Запись глазами гостя (ссылка управления #/m/<токен>). */
export interface GuestBooking {
  page_title: string;
  /** Адрес страницы записи — пусто, если запись на ней сейчас закрыта. */
  page_slug: string;
  owner_name: string;
  owner_timezone: string;
  duration_minutes: number;
  guest_name: string;
  /** Копия извещений (F5), замаскирована: «m***@example.com»; "" — нет. */
  cc_email?: string;
  /** Запись по пакету занятий (F5). */
  by_package?: boolean;
  /** Цена встречи на момент записи (0 — по абонементу или бесплатно); старый сервер — нет поля. */
  price?: number;
  currency?: Currency | string;
  status: BookingStatus;
  cancelled_by: CancelledBy;
  occurrences: Occurrence[];
  /** Старое поле: равно can_reschedule (оставлено для совместимости). */
  can_change: boolean;
  /**
    Можно отменить: запись активна и есть предстоящая встреча — даже если страница
    на паузе или удалена. Старый сервер не присылает — тогда смотрим can_change.
  */
  can_cancel?: boolean;
  /** Можно перенести: страница открыта, запись активна, есть предстоящая встреча и переносы. */
  can_reschedule?: boolean;
  reschedules_left: number;
  /** Свободное время для переноса ближайшей встречи. */
  slots: InviteSlot[];
  slots_unavailable: boolean;
  ref: string;
  branding?: Branding;
  /** Ссылка подписки на напоминания в Telegram (F2); "" — бот не настроен или запись отменена. */
  telegram_url?: string;
  /** К записи уже подключён чат Telegram. */
  telegram_linked?: boolean;
  /** Оплата без provider_id; null — оплаты нет. */
  payment?: Payment | null;
  /** До ближайшей встречи меньше cancel_hours часов. */
  late?: boolean;
  policy?: Policy;
  payment_note?: string;
}

export interface PageFields {
  title: string;
  description?: string;
  duration_minutes: number;
  days_ahead: number;
  max_per_day: number;
  notice_hours: number;
  max_series: number;
  weekends: boolean;
  guest_reminders?: boolean;
  hide_branding?: boolean;
}

/** Всё, что редактирует форма страницы: основные поля, цена и политика отмены, пакеты (F5). */
export type PageForm = PageFields & PriceFields & { guest_reminders: boolean; hide_branding: boolean; packages: PackageOffer[] };

export interface BookRequest {
  start: string;
  /** С пакетом (package_token) можно не передавать — сервер возьмёт имя из пакета. */
  guest_name?: string;
  guest_email?: string;
  /** Копия напоминаний (F5): ≠ guest_email. */
  cc_email?: string;
  comment?: string;
  /** Встреч в еженедельной серии (1 — одна встреча). */
  count?: number;
  /** Купить пакет при записи: id предложения страницы (F5). */
  package_id?: number;
  /** Записаться по ссылке пакета (F5). Вместе с package_id нельзя. */
  package_token?: string;
}

export interface Booked {
  booking: GuestBooking;
  /** 43 символа base64url; показывается только сейчас. */
  token: string;
  /** Готовая ссылка управления или "" — если на сервере не задан SELFVOY_PUBLIC_URL. */
  url: string;
  /** Ссылка на бота для напоминаний или "" (F2). */
  telegram_url?: string;
  /** Купили пакет (package_id): токен ссылки пакета — показывается один раз (F5). */
  package_token?: string;
  /** Готовая ссылка пакета #/p/<токен> или "" (нет SELFVOY_PUBLIC_URL). */
  package_url?: string;
  /** Пакет глазами гостя — при покупке и при записи по пакету. */
  package?: GuestPackage;
}

export interface ReferralInfo {
  code: string;
  url: string;
  joined: number;
  verified: number;
  /** Начислено наград пригласившему (≤ reward_cap), F3. */
  rewarded?: number;
  reward_days?: number;
  reward_cap?: number;
  plan?: Plan;
  referrals: { display_name: string; source: 'link' | 'booking' | 'invite'; joined_at: string; verified: boolean }[];
}

export type PostFormat = 'telegram' | 'vk' | 'plain';

/** Пост «свободные окна» (F6): url — ссылка страницы или "" (клиент добавляет сам). */
export interface SlotsPost {
  text: string;
  slots: number;
  url: string;
}

/** Подбор свободного времени идёт через ИИ-сервис — гостевым запросам даём больше времени. */
const SLOTS_TIMEOUT_MS = 60_000;

export function makeBookingApi(socket: SelfvoySocket) {
  const call = <T,>(type: string, payload: Record<string, unknown> = {}, timeout?: number) =>
    timeout === undefined ? socket.request<T>(type, payload) : socket.request<T>(type, payload, timeout);
  return {
    // -------- владелец
    createPage: (fields: PageFields | PageForm) => call<{ page: BookingPage }>('booking.pageCreate', { ...fields }),
    listPages: () => call<{ pages: BookingPage[] }>('booking.pageList'),
    updatePage: (page_id: number, fields: Partial<PageForm> & { status?: 'active' | 'paused' }) =>
      call<{ page: BookingPage }>('booking.pageUpdate', { page_id, ...fields }),
    deletePage: (page_id: number) => call<{ deleted: boolean }>('booking.pageDelete', { page_id }),
    list: (limit = 50) => call<{ bookings: Booking[] }>('booking.list', { limit }),
    ownerCancel: (booking_id: number, occurrence?: string) => {
      const payload: Record<string, unknown> = { booking_id };
      if (occurrence) payload.occurrence = occurrence;
      return call<{ booking: Booking }>('booking.ownerCancel', payload);
    },
    /** Отметка оплаты владельцем: paid / waived (без оплаты) / refunded. */
    paymentSet: (booking_id: number, status: OwnerPaymentStatus) => call<{ booking: Booking }>('booking.paymentSet', { booking_id, status }),
    /** Текст поста со свободными окнами страницы. Окна подбирает ИИ-сервис — ждём дольше. */
    slotsPost: (page_id: number, options: { days?: number; format?: PostFormat } = {}) => {
      const payload: Record<string, unknown> = { page_id };
      if (options.days) payload.days = options.days;
      if (options.format) payload.format = options.format;
      return call<SlotsPost>('booking.slotsPost', payload, SLOTS_TIMEOUT_MS);
    },
    referral: () => call<ReferralInfo>('referral.get'),

    // -------- гость (публичные методы)
    get: (slug: string) => call<{ page: GuestPage }>('booking.get', { slug }, SLOTS_TIMEOUT_MS),
    book: (slug: string, request: BookRequest) => {
      const payload: Record<string, unknown> = { slug, start: request.start };
      if (request.guest_name || !request.package_token) payload.guest_name = request.guest_name ?? '';
      if (request.guest_email) payload.guest_email = request.guest_email;
      if (request.cc_email) payload.cc_email = request.cc_email;
      if (request.comment) payload.comment = request.comment;
      if (request.count && request.count > 1) payload.count = request.count;
      if (request.package_token) payload.package_token = request.package_token;
      else if (request.package_id) payload.package_id = request.package_id;
      return call<Booked>('booking.book', payload, SLOTS_TIMEOUT_MS);
    },
    manage: (token: string) => call<{ booking: GuestBooking }>('booking.manage', { token }, SLOTS_TIMEOUT_MS),
    reschedule: (token: string, start: string, occurrence?: string) => {
      const payload: Record<string, unknown> = { token, start };
      if (occurrence) payload.occurrence = occurrence;
      return call<{ booking: GuestBooking }>('booking.reschedule', payload, SLOTS_TIMEOUT_MS);
    },
    /** Потерянная ссылка: новая уходит на email гостя (ответ одинаков, есть ли запись). */
    resendLink: (slug: string, email: string) => call<{ sent: boolean }>('booking.resendLink', { slug, email }),
    cancel: (token: string, options: { occurrence?: string; reason?: string } = {}) => {
      const payload: Record<string, unknown> = { token };
      if (options.occurrence) payload.occurrence = options.occurrence;
      if (options.reason) payload.reason = options.reason;
      return call<{ booking: GuestBooking }>('booking.cancel', payload);
    },
    /** «Приду»: без occurrence — ближайшая предстоящая встреча. Повтор ничего не меняет. */
    confirm: (token: string, occurrence?: string) => {
      const payload: Record<string, unknown> = { token };
      if (occurrence) payload.occurrence = occurrence;
      return call<{ booking: GuestBooking }>('booking.confirm', payload);
    },
    /** «Я оплатил(а)» для оплаты по ссылке: pending → claimed. */
    paymentClaim: (token: string) => call<{ booking: GuestBooking }>('booking.paymentClaim', { token }),
  };
}

export type BookingApi = ReturnType<typeof makeBookingApi>;
