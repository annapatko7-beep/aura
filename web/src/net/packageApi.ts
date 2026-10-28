/*
  Пакеты занятий и абонементы (F5, docs/PROTOCOL.md → «Пакеты, абонементы и второй
  получатель»).

  Владелец (нужен вход): package.list / issue / adjust / cancel / paymentSet.
  Гость (без входа): package.get / paymentClaim — по токену ссылки пакета #/p/<токен>;
  токен сервер показывает один раз (в ответе booking.book с package_id или
  package.issue), в БД только хэш.

  Предложения пакетов — поле страницы `packages` (booking.pageCreate / pageUpdate,
  правка целым массивом), покупка и запись по пакету — booking.book (bookingApi.ts).
*/
import type { Branding, Currency, OwnerPaymentStatus, Payment } from './bookingApi';
import type { SelfvoySocket } from './socket';

/** Предложение пакета на странице записи. id нет — новое (сервер выдаст). */
export interface PackageOffer {
  id?: number;
  title: string;
  /** Занятий в пакете: 2–100. */
  sessions: number;
  /** Цена в минимальных единицах валюты страницы: > 0. */
  price: number;
  /** Срок действия после оплаты: 7–365 дней (90 по умолчанию). */
  valid_days?: number;
}

export type PackageStatus = 'pending' | 'active' | 'used' | 'expired' | 'cancelled';

/** Пакет глазами владельца. */
export interface OwnerPackage {
  id: number;
  page_id: number;
  page_title: string;
  /** 0 — выдан вне предложений страницы. */
  offer_id: number;
  title: string;
  sessions_total: number;
  sessions_used: number;
  sessions_left: number;
  valid_days: number;
  /** "" — ещё не оплачен (срок начнётся после оплаты). */
  valid_until: string;
  price: number;
  currency: Currency | string;
  status: PackageStatus;
  payment: Payment | null;
  guest_name: string;
  guest_email: string;
  cc_email: string;
  created_at: string;
  updated_at: string;
}

/** Запись по пакету глазами гостя: только время и статус. */
export interface GuestPackageBooking {
  start: string;
  status: 'active' | 'cancelled';
}

/** Пакет глазами гостя (#/p/<токен>). */
export interface GuestPackage {
  title: string;
  sessions_total: number;
  sessions_left: number;
  valid_until: string;
  valid_days: number;
  status: PackageStatus;
  price: number;
  currency: Currency | string;
  /** Без provider_id; null — без оплаты. */
  payment: Payment | null;
  payment_note?: string;
  guest_name: string;
  /** Замаскирован: «m***@example.com»; "" — нет. */
  cc_email?: string;
  /** "" — запись на странице закрыта. */
  page_slug: string;
  page_title: string;
  owner_name: string;
  owner_timezone: string;
  bookings: GuestPackageBooking[];
  branding?: Branding;
  /** Реферальный код владельца — для ссылки «Запись через Selfvoy» (как booking.get / invite.get). */
  ref?: string;
}

/** package.issue: готовое предложение страницы или своё (название + занятия). */
export type IssueRequest = {
  page_id: number;
  guest_name: string;
  guest_email?: string;
  cc_email?: string;
  /** Оплата уже получена — пакет сразу «оплачен». */
  paid?: boolean;
} & ({ offer_id: number } | { title: string; sessions: number; price?: number; valid_days?: number });

export interface Issued {
  package: OwnerPackage;
  /** Показывается один раз. */
  token: string;
  /** Готовая ссылка пакета или "" (нет SELFVOY_PUBLIC_URL). */
  url: string;
}

export function makePackageApi(socket: SelfvoySocket) {
  const call = <T,>(type: string, payload: Record<string, unknown> = {}) => socket.request<T>(type, payload);
  return {
    // -------- владелец
    list: (options: { page_id?: number; status?: PackageStatus; limit?: number } = {}) => {
      const payload: Record<string, unknown> = {};
      if (options.page_id) payload.page_id = options.page_id;
      if (options.status) payload.status = options.status;
      if (options.limit) payload.limit = options.limit;
      return call<{ packages: OwnerPackage[] }>('package.list', payload);
    },
    issue: (request: IssueRequest) => {
      const payload: Record<string, unknown> = { page_id: request.page_id, guest_name: request.guest_name };
      if ('offer_id' in request && request.offer_id) {
        payload.offer_id = request.offer_id;
      } else if ('title' in request) {
        payload.title = request.title;
        payload.sessions = request.sessions;
        if (request.price) payload.price = request.price;
        if (request.valid_days) payload.valid_days = request.valid_days;
      }
      if (request.guest_email) payload.guest_email = request.guest_email;
      if (request.cc_email) payload.cc_email = request.cc_email;
      if (request.paid) payload.paid = true;
      return call<Issued>('package.issue', payload);
    },
    /** Корректировка остатка: delta −100..100, не 0. */
    adjust: (package_id: number, delta: number) => call<{ package: OwnerPackage }>('package.adjust', { package_id, delta }),
    cancel: (package_id: number) => call<{ package: OwnerPackage }>('package.cancel', { package_id }),
    paymentSet: (package_id: number, status: OwnerPaymentStatus) => call<{ package: OwnerPackage }>('package.paymentSet', { package_id, status }),

    // -------- гость (публичные методы)
    get: (token: string) => call<{ package: GuestPackage }>('package.get', { token }),
    /** «Я оплатил(а)» за пакет (оплата по ссылке): pending → claimed. */
    paymentClaim: (token: string) => call<{ package: GuestPackage }>('package.paymentClaim', { token }),
  };
}

export type PackageApi = ReturnType<typeof makePackageApi>;
