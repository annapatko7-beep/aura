/*
  Страницы записи и рефералы: чистая логика без React (тестируется в booking.test.ts).
  Время, группировка слотов по дням, .ics и тексты ошибок гостя — общие с
  приглашениями (lib/invite.ts).
*/
import type { Booking, BookingPage, GuestBooking, Occurrence, PageFields, PageForm, PostFormat, PriceFields } from '../net/bookingApi';
import { SelfvoyError, isNetworkError } from '../net/socket';
import { plural } from './format';
import { PRO_REQUIRED_TEXT } from './integrations';
import { foldIcsLine, icsEscape, icsUtc, isPublicWebOrigin, type LocationLike } from './invite';
import { PRICE_MAX } from './money';
import { cleanOffers, offersEqual, offersError } from './packages';
import type { PackageOffer } from '../net/packageApi';

// ------------------------------------------------------------------ ссылки

/** Адрес страницы записи: 10 символов [a-z0-9] (как выдаёт сервер). */
export const isPlausibleSlug = (slug: string | undefined): slug is string => Boolean(slug && /^[a-z0-9]{10}$/.test(slug));

/**
  Публичная ссылка на путь веб-клиента (#/b/<slug>, #/m/<токен>, #/signup?ref=…):
  готовая от сервера (SELFVOY_PUBLIC_URL) — как есть; иначе адрес этого веб-клиента,
  если он публичный; иначе null (Electron/Capacitor/localhost — ссылку не собрать).
*/
export function publicLink(path: string, serverUrl?: string | null, loc?: LocationLike | null | undefined): string | null {
  const url = (serverUrl ?? '').trim();
  if (/^https?:\/\//i.test(url)) return url;
  if (loc && isPublicWebOrigin(loc)) return `${loc.origin}${loc.pathname}#${path}`;
  return null;
}

export const pagePath = (slug: string) => `/b/${slug}`;
export const managePath = (token: string) => `/m/${token}`;
export const signupPath = (ref: string, source: 'link' | 'booking' | 'invite' = 'link') =>
  ref ? `/signup?ref=${encodeURIComponent(ref)}${source === 'link' ? '' : `&src=${source}`}` : '/signup';

// ------------------------------------------------------------------ рефералы

const REF_KEY = 'selfvoy.ref';

/** Код из адреса регистрации: 8 букв/цифр; иначе null (мусор не отправляем на сервер). */
export function parseRef(search: URLSearchParams): { code: string; source: 'link' | 'booking' | 'invite' } | null {
  const code = (search.get('ref') ?? '').trim().toUpperCase();
  if (!/^[A-Z0-9]{8}$/.test(code)) return null;
  const src = search.get('src');
  return { code, source: src === 'booking' || src === 'invite' ? src : 'link' };
}

/** Код переживает переход «Регистрация → Вход → Регистрация» внутри вкладки. */
export function rememberRef(ref: { code: string; source: string } | null): void {
  try {
    if (ref) sessionStorage.setItem(REF_KEY, JSON.stringify(ref));
  } catch {
    /* хранилище недоступно (приватный режим) — код просто не запомнится */
  }
}

export function recalledRef(): { code: string; source: 'link' | 'booking' | 'invite' } | null {
  try {
    const raw = sessionStorage.getItem(REF_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as { code?: unknown; source?: unknown };
    return parseRef(new URLSearchParams({ ref: String(parsed.code ?? ''), src: String(parsed.source ?? '') }));
  } catch {
    return null;
  }
}

export function forgetRef(): void {
  try {
    sessionStorage.removeItem(REF_KEY);
  } catch {
    /* нечего чистить */
  }
}

// ------------------------------------------------------------------ формы

export const TITLE_MAX = 120;
export const DESCRIPTION_MAX = 1000;

export const PAYMENT_URL_MAX = 500;
export const PAYMENT_NOTE_MAX = 300;
export const POLICY_MAX = 500;

/** Цена и политика по умолчанию — как на сервере (F4). */
export const PRICE_DEFAULTS: PriceFields = {
  price: 0,
  currency: 'RUB',
  prepay: 'none',
  deposit: 0,
  payment_url: '',
  payment_note: '',
  pay_within_minutes: 60,
  cancel_hours: 0,
  late_cancel: 'allow',
  policy_text: '',
};

export const PAGE_DEFAULTS: PageForm = {
  title: '',
  description: '',
  duration_minutes: 30,
  days_ahead: 14,
  max_per_day: 4,
  notice_hours: 2,
  max_series: 1,
  weekends: false,
  guest_reminders: true,
  hide_branding: false,
  ...PRICE_DEFAULTS,
  packages: [],
};

export interface PageFormErrors {
  title?: string;
  description?: string;
  numbers?: string;
  price?: string;
  payment?: string;
  policy?: string;
  /** Пакеты и абонементы (F5). */
  packages?: string;
}

const length = (s: string) => [...s].length;
const inRange = (n: number, min: number, max: number) => Number.isInteger(n) && n >= min && n <= max;

/**
  Те же пределы, что на сервере (BookingManager::pageCreate). `yookassa` — у владельца
  подключена ЮKassa: тогда предоплате не нужны ни ссылка, ни реквизиты.
*/
export function pageFormErrors(form: PageFields & Partial<PriceFields> & { packages?: PackageOffer[] }, options: { yookassa?: boolean } = {}): PageFormErrors {
  const errors: PageFormErrors = {};
  const title = form.title.trim();
  if (!title) errors.title = 'Назовите страницу — например, «Консультация»';
  else if (length(title) > TITLE_MAX || /[\r\n]/.test(title)) errors.title = `Название — до ${TITLE_MAX} символов в одну строку`;
  if (length((form.description ?? '').trim()) > DESCRIPTION_MAX) errors.description = `Описание — до ${DESCRIPTION_MAX} символов`;
  if (
    !inRange(form.duration_minutes, 15, 240) ||
    !inRange(form.days_ahead, 1, 60) ||
    !inRange(form.max_per_day, 1, 16) ||
    !inRange(form.notice_hours, 0, 168) ||
    !inRange(form.max_series, 1, 12)
  ) {
    errors.numbers = 'Проверьте числа: длительность 15–240 мин, запись на 1–60 дней, до 16 встреч в день, запас 0–168 ч, серия до 12 встреч';
  }
  const price = form.price ?? 0;
  const prepay = form.prepay ?? 'none';
  if (!inRange(price, 0, PRICE_MAX)) errors.price = price > PRICE_MAX ? 'Цена — не больше 1 000 000' : 'Проверьте цену: например, 2500 или 2500,50';
  else if (prepay !== 'none' && price <= 0) errors.price = 'Для предоплаты укажите цену';
  else if (prepay === 'deposit' && !inRange(form.deposit ?? 0, 1, price)) errors.price = 'Депозит — больше нуля и не больше цены';
  const url = (form.payment_url ?? '').trim();
  const note = (form.payment_note ?? '').trim();
  if (url && (!/^https:\/\/[^\s]+$/i.test(url) || url.length > PAYMENT_URL_MAX)) errors.payment = `Ссылка оплаты — только https://, без пробелов, до ${PAYMENT_URL_MAX} символов`;
  else if (length(note) > PAYMENT_NOTE_MAX || /[\r\n]/.test(note)) errors.payment = `Реквизиты — одна строка до ${PAYMENT_NOTE_MAX} символов`;
  else if (!inRange(form.pay_within_minutes ?? 60, 15, 1440)) errors.payment = 'Срок оплаты — от 15 минут до 24 часов';
  else if (prepay !== 'none' && !url && !note && !options.yookassa) errors.payment = 'Укажите, как платить: ссылку оплаты, реквизиты или подключите ЮKassa';
  if (!inRange(form.cancel_hours ?? 0, 0, 168)) errors.policy = 'Срок отмены — от 0 до 168 часов';
  else if (length((form.policy_text ?? '').trim()) > POLICY_MAX) errors.policy = `Текст политики — до ${POLICY_MAX} символов`;
  const offers = offersError(form.packages);
  if (offers) errors.packages = offers.text;
  return errors;
}

/**
  Только изменённые поля — pageUpdate меняет переданные (включение Pro-функций проверяется
  отдельно). Пакеты сравниваются по содержимому и уходят целым массивом (F5).
*/
export function changedFields(before: PageForm, after: PageForm): Partial<PageForm> {
  const out: Record<string, unknown> = {};
  for (const key of Object.keys(after) as (keyof PageForm)[]) {
    if (key === 'packages') {
      if (!offersEqual(before.packages, after.packages)) out.packages = cleanOffers(after.packages);
    } else if (after[key] !== before[key]) out[key] = after[key];
  }
  return out as Partial<PageForm>;
}

/** Поля страницы для формы редактирования (поля, которых нет у старого сервера, — по умолчанию). */
export const pageFields = (page: BookingPage): PageForm => ({
  title: page.title,
  description: page.description,
  duration_minutes: page.duration_minutes,
  days_ahead: page.days_ahead,
  max_per_day: page.max_per_day,
  notice_hours: page.notice_hours,
  max_series: page.max_series,
  weekends: page.weekends,
  guest_reminders: page.guest_reminders ?? true,
  hide_branding: page.hide_branding ?? false,
  price: page.price ?? PRICE_DEFAULTS.price,
  currency: page.currency ?? PRICE_DEFAULTS.currency,
  prepay: page.prepay ?? PRICE_DEFAULTS.prepay,
  deposit: page.deposit ?? PRICE_DEFAULTS.deposit,
  payment_url: page.payment_url ?? '',
  payment_note: page.payment_note ?? '',
  pay_within_minutes: page.pay_within_minutes ?? PRICE_DEFAULTS.pay_within_minutes,
  cancel_hours: page.cancel_hours ?? PRICE_DEFAULTS.cancel_hours,
  late_cancel: page.late_cancel ?? PRICE_DEFAULTS.late_cancel,
  policy_text: page.policy_text ?? '',
  packages: (page.packages ?? []).map((o) => ({ ...o })),
});

// ------------------------------------------------------------------ ссылка управления

/**
  Параметры ссылки управления #/m/<токен>?confirm=<start>&paid=1: «Приду» из
  напоминания (F1) и возврат после оплаты (F4). confirm — только разборчивое время.
*/
export function parseManageQuery(search: string | URLSearchParams): { confirm: string | null; paid: boolean } {
  const params = typeof search === 'string' ? new URLSearchParams(search.startsWith('?') ? search.slice(1) : search) : search;
  const raw = (params.get('confirm') ?? '').trim();
  const confirm = raw && raw.length <= 40 && !Number.isNaN(Date.parse(raw)) ? raw : null;
  return { confirm, paid: params.get('paid') === '1' };
}

/** Встреча, которую подтверждает ссылка: точное совпадение start или тот же момент времени. */
export function occurrenceAt(list: Occurrence[] | undefined, start: string): Occurrence | null {
  const at = Date.parse(start);
  return (list ?? []).find((o) => o.start === start || Date.parse(o.start) === at) ?? null;
}

// ------------------------------------------------------------------ пост «свободные окна»

export const POST_FORMATS: readonly { value: PostFormat; label: string }[] = [
  { value: 'telegram', label: 'Telegram' },
  { value: 'vk', label: 'VK' },
  { value: 'plain', label: 'Текст' },
];

/**
  Сервер без SELFVOY_PUBLIC_URL присылает пост без строки со ссылкой (url ""): клиент
  добавляет её сам в том же формате, что сервер (vk — «Запись онлайн», plain — без пустой строки).
*/
export function withPostLink(text: string, format: PostFormat, serverUrl: string, link: string | null): string {
  if (serverUrl || !link) return text;
  const body = text.replace(/\s+$/, '');
  if (format === 'vk') return `${body}\n\nЗапись онлайн: ${link}`;
  if (format === 'plain') return `${body}\nЗаписаться: ${link}`;
  return `${body}\n\nЗаписаться: ${link}`;
}

// ------------------------------------------------------------------ встречи

/** Встречи по порядку; прошедшие помечаются, отменённые остаются (видно историю). */
export function sortedOccurrences(list: Occurrence[] | undefined): Occurrence[] {
  return [...(list ?? [])].filter((o) => !Number.isNaN(Date.parse(o?.start))).sort((a, b) => Date.parse(a.start) - Date.parse(b.start));
}

export const isUpcoming = (o: Occurrence, now = new Date()) => o.status === 'active' && Date.parse(o.start) > now.getTime();

/** Ближайшая предстоящая встреча (её по умолчанию переносит booking.reschedule). */
export function nextOccurrence(list: Occurrence[] | undefined, now = new Date()): Occurrence | null {
  return sortedOccurrences(list).find((o) => isUpcoming(o, now)) ?? null;
}

export function upcomingCount(list: Occurrence[] | undefined, now = new Date()): number {
  return (list ?? []).filter((o) => isUpcoming(o, now)).length;
}

/**
  Что гость может сделать со своей записью (booking.manage). can_cancel — запись
  активна и есть предстоящая встреча (даже если страница на паузе или удалена);
  can_reschedule — ещё и страница открыта, и переносы не кончились. Старый сервер
  присылает только can_change (= can_reschedule) — тогда берём его для обоих.
*/
export function guestActions(
  booking: Pick<GuestBooking, 'can_change' | 'can_cancel' | 'can_reschedule' | 'reschedules_left'>,
): { canCancel: boolean; canReschedule: boolean } {
  const fallback = Boolean(booking.can_change);
  const canCancel = typeof booking.can_cancel === 'boolean' ? booking.can_cancel : fallback;
  const reschedule = typeof booking.can_reschedule === 'boolean' ? booking.can_reschedule : fallback;
  return { canCancel, canReschedule: reschedule && canCancel && booking.reschedules_left > 0 };
}

/** «Одна встреча» / «Серия: 4 встречи, раз в неделю». */
export function seriesLabel(count: number): string {
  if (count <= 1) return 'Одна встреча';
  return `Серия: ${count} ${plural(count, 'встреча', 'встречи', 'встреч')}, раз в неделю`;
}

/** Сколько раз гость может перенести запись (kMaxReschedules на сервере). */
export const MAX_RESCHEDULES = 3;

export const PAGE_STATUS_LABEL: Record<string, string> = { active: 'Открыта', paused: 'На паузе', archived: 'Удалена' };

/** Статус записи для владельца: «Предстоит» / «Прошла» / «Отменил гость» / «Отменили вы». */
export function bookingStatus(booking: Pick<Booking, 'status' | 'cancelled_by' | 'occurrences'>, now = new Date()): { label: string; tone: string } {
  if (booking.status === 'cancelled') {
    if (booking.cancelled_by === 'system') return { label: 'Отменена: нет оплаты', tone: 'red' };
    return { label: booking.cancelled_by === 'owner' ? 'Отменили вы' : 'Отменил гость', tone: 'red' };
  }
  return upcomingCount(booking.occurrences, now) > 0 ? { label: 'Предстоит', tone: 'green' } : { label: 'Прошла', tone: '' };
}

// ------------------------------------------------------------------ .ics

/** FNV-1a (32 бита) с заданным началом — две разные затравки дают 64 бита. */
function fnv1a(text: string, seed: number): string {
  let hash = seed >>> 0;
  for (const ch of text) {
    hash ^= ch.codePointAt(0) ?? 0;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash.toString(16).padStart(8, '0');
}

/**
  UID встречи записи — один и тот же до и после переноса, чтобы календарь обновил
  событие, а не завёл второе. Основа — ключ записи (токен ссылки управления: он есть
  у гостя и на экране «Вы записаны», и на #/m/<токен>) и номер встречи в записи
  (сервер переносит встречу на её же месте в списке). Время в UID не входит.
  Токен в файл не попадает — только его хэш.
*/
export function bookingUid(key: string, index: number): string {
  const base = `${key}\u0000${index}`;
  return `booking-${fnv1a(base, 0x811c9dc5)}${fnv1a(base, 0x050c5d1f)}-${index}@selfvoy`;
}

/**
  Календарь со всеми активными встречами записи (одна или серия). `key` — токен
  ссылки управления (см. bookingUid). SEQUENCE растёт с каждым переносом.
*/
export function bookingIcs(
  booking: Pick<GuestBooking, 'page_title' | 'owner_name' | 'occurrences'> & Partial<Pick<GuestBooking, 'reschedules_left'>>,
  key: string,
  now = new Date(),
): string {
  const owner = booking.owner_name.trim();
  const topic = booking.page_title.trim() || 'Встреча';
  // Номер встречи — по порядку сервера (до сортировки): он не меняется при переносе.
  const events = (booking.occurrences ?? [])
    .map((occurrence, index) => ({ occurrence, index }))
    .filter(({ occurrence }) => occurrence?.status === 'active' && !Number.isNaN(Date.parse(occurrence.start)))
    .sort((a, b) => Date.parse(a.occurrence.start) - Date.parse(b.occurrence.start));
  if (events.length === 0) throw new Error('Нет встреч для календаря');
  const left = booking.reschedules_left;
  const sequence = typeof left === 'number' && Number.isFinite(left) ? Math.max(0, MAX_RESCHEDULES - left) : 0;
  const lines = ['BEGIN:VCALENDAR', 'VERSION:2.0', 'PRODID:-//Selfvoy//Booking//RU', 'CALSCALE:GREGORIAN', 'METHOD:PUBLISH'];
  for (const { occurrence: event, index } of events) {
    const start = new Date(event.start);
    const end = new Date(event.end);
    if (Number.isNaN(start.getTime()) || Number.isNaN(end.getTime())) continue;
    lines.push(
      'BEGIN:VEVENT',
      `UID:${icsEscape(bookingUid(key, index))}`,
      `SEQUENCE:${sequence}`,
      `DTSTAMP:${icsUtc(now)}`,
      `DTSTART:${icsUtc(start)}`,
      `DTEND:${icsUtc(end)}`,
      `SUMMARY:${icsEscape(owner ? `${topic} — ${owner}` : topic)}`,
      `DESCRIPTION:${icsEscape(owner ? `Запись к ${owner} через Selfvoy` : 'Запись через Selfvoy')}`,
      'END:VEVENT',
    );
  }
  lines.push('END:VCALENDAR');
  return lines.map(foldIcsLine).join('\r\n') + '\r\n';
}

// ------------------------------------------------------------------ ошибки

const sentence = (text: string) => (text ? text.charAt(0).toUpperCase() + text.slice(1) : text);

/** Ошибка booking.page* / list / ownerCancel / referral.get для владельца. */
export function bookingOwnerErrorText(error: unknown): string {
  if (!(error instanceof SelfvoyError)) return 'Что-то пошло не так';
  if (error.code === 'timeout') return 'Сервер не ответил вовремя — попробуйте ещё раз';
  if (isNetworkError(error)) return 'Нет связи с сервером Selfvoy';
  if (error.code === 'pro_required') return PRO_REQUIRED_TEXT;
  if (error.code === 'upstream_error') return sentence(error.message) || 'Selfvoy сейчас не может подобрать время — попробуйте позже';
  return sentence(error.message) || 'Что-то пошло не так';
}
