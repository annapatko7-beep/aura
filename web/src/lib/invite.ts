/*
  Приглашение по ссылке: чистая логика без React — ссылка для гостя, группировка
  слотов по дням в часовом поясе браузера, файл .ics, подписи статусов, проверка
  форм и тексты ошибок. Проверяется юнит-тестами (invite.test.ts).
*/
import type { GuestInvite, Invite, InviteSlot, InviteStatus } from '../net/inviteApi';
import { SelfvoyError, isNetworkError } from '../net/socket';
import { nameError } from './validate';

// ------------------------------------------------------------------ статусы

export const INVITE_STATUS_LABEL: Record<InviteStatus, string> = {
  open: 'Открыто',
  accepted: 'Время выбрано',
  declined: 'Отказ',
  cancelled: 'Отменено',
  expired: 'Истекло',
};

const STATUS_TONE: Record<InviteStatus, '' | 'accent' | 'green' | 'red'> = {
  open: 'accent',
  accepted: 'green',
  declined: 'red',
  cancelled: '',
  expired: '',
};

const KNOWN_STATUS = new Set<string>(Object.keys(INVITE_STATUS_LABEL));

/**
  Действующий статус: сервер сам вычисляет expired, но список мог устареть, пока
  страница открыта, — открытое приглашение с прошедшим сроком показываем истёкшим.
*/
export function inviteStatus(invite: Pick<Invite | GuestInvite, 'status' | 'expires_at'>, now = new Date()): InviteStatus {
  const status = KNOWN_STATUS.has(invite.status) ? invite.status : 'expired';
  if (status === 'open') {
    const expires = Date.parse(invite.expires_at);
    if (!Number.isNaN(expires) && expires <= now.getTime()) return 'expired';
  }
  return status;
}

export const statusLabel = (status: InviteStatus) => INVITE_STATUS_LABEL[status] ?? status;
export const statusTone = (status: InviteStatus) => STATUS_TONE[status] ?? '';

/** Выбранное время: сервер шлёт `{}`, пока его нет. */
export function chosenSlot(invite: { chosen?: Partial<InviteSlot> | null }): InviteSlot | null {
  const c = invite.chosen;
  if (!c || !c.start || !c.end) return null;
  if (Number.isNaN(Date.parse(c.start)) || Number.isNaN(Date.parse(c.end))) return null;
  return { start: c.start, end: c.end };
}

// ------------------------------------------------------------------ ссылка

export interface LocationLike {
  protocol: string;
  hostname: string;
  origin: string;
  pathname: string;
}

export type InviteLink = { kind: 'url'; url: string } | { kind: 'token'; token: string };

/** Адрес, который откроется у другого человека: http(s) и не localhost/loopback (Capacitor — https://localhost). */
export function isPublicWebOrigin(loc: LocationLike | null | undefined): boolean {
  if (!loc || (loc.protocol !== 'http:' && loc.protocol !== 'https:')) return false;
  const host = loc.hostname.toLowerCase().replace(/^\[|\]$/g, '');
  if (!host) return false;
  if (host === 'localhost' || host.endsWith('.localhost')) return false;
  if (host === '::1' || host === '0.0.0.0' || /^127\./.test(host)) return false;
  return true;
}

/**
  Ссылка для гостя: готовая от сервера (SELFVOY_PUBLIC_URL), иначе — адрес этого
  веб-клиента, если он публичный; иначе — только токен (страница подскажет, что
  на сервере нужен SELFVOY_PUBLIC_URL).
*/
export function inviteLink(token: string, serverUrl: string, loc: LocationLike | null | undefined): InviteLink {
  const url = serverUrl.trim();
  if (/^https?:\/\//i.test(url)) return { kind: 'url', url };
  if (loc && isPublicWebOrigin(loc)) return { kind: 'url', url: `${loc.origin}${loc.pathname}#/i/${token}` };
  return { kind: 'token', token };
}

/** Токен из адреса: base64url; заведомо битый не отправляем на сервер. */
export function isPlausibleToken(token: string | undefined): token is string {
  return Boolean(token && /^[A-Za-z0-9_-]{16,256}$/.test(token));
}

// ------------------------------------------------------------------ время

export function browserTimeZone(): string {
  try {
    return Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC';
  } catch {
    return 'UTC';
  }
}

function safeZone(timeZone: string): string {
  try {
    new Intl.DateTimeFormat('ru-RU', { timeZone });
    return timeZone;
  } catch {
    return 'UTC';
  }
}

/** Ключ дня «ГГГГ-ММ-ДД» в часовом поясе. */
export function dayKey(date: Date, timeZone: string): string {
  const parts = new Intl.DateTimeFormat('en-CA', { timeZone: safeZone(timeZone), year: 'numeric', month: '2-digit', day: '2-digit' }).formatToParts(date);
  const get = (type: string) => parts.find((p) => p.type === type)?.value ?? '';
  return `${get('year')}-${get('month')}-${get('day')}`;
}

function nextDayKey(key: string): string {
  const [y, m, d] = key.split('-').map(Number);
  const next = new Date(Date.UTC(y, m - 1, d + 1));
  return `${next.getUTCFullYear()}-${String(next.getUTCMonth() + 1).padStart(2, '0')}-${String(next.getUTCDate()).padStart(2, '0')}`;
}

/** «10:00» в часовом поясе. */
export function timeIn(date: Date, timeZone: string): string {
  return new Intl.DateTimeFormat('ru-RU', { timeZone: safeZone(timeZone), hour: '2-digit', minute: '2-digit', hourCycle: 'h23' }).format(date);
}

/** «четверг, 15 октября» в часовом поясе. */
function dateIn(date: Date, timeZone: string): string {
  return new Intl.DateTimeFormat('ru-RU', { timeZone: safeZone(timeZone), weekday: 'long', day: 'numeric', month: 'long' }).format(date);
}

const capitalize = (s: string) => (s ? s[0].toUpperCase() + s.slice(1) : s);

/** «Сегодня, 15 октября» / «Завтра, 16 октября» / «Суббота, 17 октября». */
export function dayTitleIn(date: Date, timeZone: string, now = new Date()): string {
  const key = dayKey(date, timeZone);
  const today = dayKey(now, timeZone);
  const full = dateIn(date, timeZone);
  const dayMonth = full.includes(',') ? full.slice(full.indexOf(',') + 1).trim() : full;
  if (key === today) return `Сегодня, ${dayMonth}`;
  if (key === nextDayKey(today)) return `Завтра, ${dayMonth}`;
  return capitalize(full);
}

export interface SlotView extends InviteSlot {
  /** «10:00–11:00» */
  label: string;
}

export interface DayGroup {
  key: string;
  title: string;
  slots: SlotView[];
}

/** Слоты по дням (в поясе гостя), хронологически; прошедшие и битые отбрасываются. */
export function groupSlotsByDay(slots: InviteSlot[], timeZone: string, now = new Date()): DayGroup[] {
  const valid = (slots ?? [])
    .map((s) => ({ s, start: Date.parse(s?.start), end: Date.parse(s?.end) }))
    .filter((x) => !Number.isNaN(x.start) && !Number.isNaN(x.end) && x.end > x.start && x.start > now.getTime())
    .sort((a, b) => a.start - b.start);
  const groups: DayGroup[] = [];
  for (const { s, start, end } of valid) {
    const startDate = new Date(start);
    const key = dayKey(startDate, timeZone);
    let group = groups.find((g) => g.key === key);
    if (!group) {
      group = { key, title: dayTitleIn(startDate, timeZone, now), slots: [] };
      groups.push(group);
    }
    group.slots.push({ start: s.start, end: s.end, label: `${timeIn(startDate, timeZone)}–${timeIn(new Date(end), timeZone)}` });
  }
  return groups;
}

/** «Суббота, 17 октября, 10:00–11:00» — выбранное время целиком. */
export function slotPhrase(slot: InviteSlot, timeZone: string, now = new Date()): string {
  const start = new Date(slot.start);
  const end = new Date(slot.end);
  if (Number.isNaN(start.getTime()) || Number.isNaN(end.getTime())) return '';
  return `${dayTitleIn(start, timeZone, now)}, ${timeIn(start, timeZone)}–${timeIn(end, timeZone)}`;
}

/** «Europe/Moscow (GMT+3)» — подпись часового пояса. */
export function timeZoneLabel(timeZone: string, at = new Date()): string {
  const zone = safeZone(timeZone);
  try {
    const offset = new Intl.DateTimeFormat('en-US', { timeZone: zone, timeZoneName: 'shortOffset' })
      .formatToParts(at)
      .find((p) => p.type === 'timeZoneName')?.value;
    return offset ? `${zone.replace(/_/g, ' ')} (${offset})` : zone.replace(/_/g, ' ');
  } catch {
    return zone;
  }
}

// ------------------------------------------------------------------ .ics

export interface IcsEvent {
  uid: string;
  start: string;
  end: string;
  summary: string;
  description?: string;
  /** Момент создания файла (DTSTAMP). */
  now?: Date;
}

/** 2026-10-15T07:00:00Z → 20261015T070000Z (всегда UTC). */
export function icsUtc(date: Date): string {
  return date.toISOString().replace(/\.\d{3}Z$/, 'Z').replace(/[-:]/g, '');
}

/** Экранирование TEXT по RFC 5545: \ ; , и переводы строк. */
export function icsEscape(text: string): string {
  return text
    .replace(/\\/g, '\\\\')
    .replace(/;/g, '\\;')
    .replace(/,/g, '\\,')
    .replace(/\r\n|\r|\n/g, '\\n');
}

/** Перенос строк длиннее 75 октетов (UTF-8), не разрывая символы; продолжение — с пробела. */
export function foldIcsLine(line: string): string {
  const encoder = new TextEncoder();
  const out: string[] = [];
  let current = '';
  let bytes = 0;
  let limit = 75;
  for (const ch of line) {
    const size = encoder.encode(ch).length;
    if (bytes + size > limit) {
      out.push(current);
      current = ' ';
      bytes = 1;
      limit = 75;
    }
    current += ch;
    bytes += size;
  }
  out.push(current);
  return out.join('\r\n');
}

/** Файл календаря с одним событием; строки через CRLF, время — в UTC. */
export function buildIcs(event: IcsEvent): string {
  const start = new Date(event.start);
  const end = new Date(event.end);
  if (Number.isNaN(start.getTime()) || Number.isNaN(end.getTime())) throw new Error('Неверное время встречи');
  const lines = [
    'BEGIN:VCALENDAR',
    'VERSION:2.0',
    'PRODID:-//Selfvoy//Invite//RU',
    'CALSCALE:GREGORIAN',
    'METHOD:PUBLISH',
    'BEGIN:VEVENT',
    `UID:${icsEscape(event.uid)}`,
    `DTSTAMP:${icsUtc(event.now ?? new Date())}`,
    `DTSTART:${icsUtc(start)}`,
    `DTEND:${icsUtc(end)}`,
    `SUMMARY:${icsEscape(event.summary)}`,
    ...(event.description ? [`DESCRIPTION:${icsEscape(event.description)}`] : []),
    'END:VEVENT',
    'END:VCALENDAR',
  ];
  return lines.map(foldIcsLine).join('\r\n') + '\r\n';
}

/** Стабильный UID без секретов (токен в файл не попадает): FNV-1a от темы, владельца и времени. */
export function inviteUid(topic: string, owner: string, start: string): string {
  let hash = 0x811c9dc5;
  for (const ch of `${topic}\u0000${owner}\u0000${start}`) {
    hash ^= ch.codePointAt(0) ?? 0;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  const at = Number.isNaN(Date.parse(start)) ? 'x' : icsUtc(new Date(start));
  return `invite-${at}-${hash.toString(16).padStart(8, '0')}@selfvoy`;
}

/** Событие календаря для гостя по принятому приглашению. */
export function guestIcs(invite: Pick<GuestInvite, 'topic' | 'note' | 'owner_name'>, slot: InviteSlot, now = new Date()): string {
  const owner = invite.owner_name.trim();
  const topic = invite.topic.trim() || 'Встреча';
  const description = [invite.note.trim(), owner ? `Организатор: ${owner}. Приглашение через Selfvoy` : 'Приглашение через Selfvoy'].filter(Boolean).join('\n\n');
  return buildIcs({
    uid: inviteUid(topic, owner, slot.start),
    start: slot.start,
    end: slot.end,
    summary: owner ? `${topic} — ${owner}` : topic,
    description,
    now,
  });
}

export function icsFileName(slot: InviteSlot): string {
  const d = new Date(slot.start);
  return Number.isNaN(d.getTime()) ? 'selfvoy-invite.ics' : `selfvoy-invite-${icsUtc(d).slice(0, 8)}.ics`;
}

// ------------------------------------------------------------------ формы

export const TOPIC_MAX = 120;
export const NOTE_MAX = 500;
export const GUEST_NAME_MAX = 80;
export const EMAIL_MAX = 254;
export const COMMENT_MAX = 500;

const length = (s: string) => [...s].length;

export interface InviteFormErrors {
  topic?: string;
  note?: string;
  guestName?: string;
}

/** Форма владельца: тема 1..120, заметка ≤ 500, имя гостя ≤ 80. */
export function inviteFormErrors(form: { topic: string; note: string; guestName: string }): InviteFormErrors {
  const errors: InviteFormErrors = {};
  const topic = form.topic.trim();
  if (!topic) errors.topic = 'Укажите тему встречи';
  else if (length(topic) > TOPIC_MAX) errors.topic = `Тема — до ${TOPIC_MAX} символов`;
  if (length(form.note.trim()) > NOTE_MAX) errors.note = `Заметка — до ${NOTE_MAX} символов`;
  if (length(form.guestName.trim()) > GUEST_NAME_MAX) errors.guestName = `Имя — до ${GUEST_NAME_MAX} символов`;
  return errors;
}

export interface GuestFormErrors {
  name?: string;
  email?: string;
  comment?: string;
  slot?: string;
}

/** Ответ гостя: имя обязательно (1..80), email и комментарий — по желанию; для accept нужен слот. */
export function guestFormErrors(form: { name: string; email: string; comment: string; slot?: string | null }, action: 'accept' | 'decline'): GuestFormErrors {
  const errors: GuestFormErrors = {};
  const name = nameError(form.name);
  if (name) errors.name = name;
  const email = form.email.trim();
  if (email) {
    if (length(email) > EMAIL_MAX || !/^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(email)) errors.email = 'Похоже, в email опечатка';
  }
  if (length(form.comment.trim()) > COMMENT_MAX) errors.comment = `Комментарий — до ${COMMENT_MAX} символов`;
  if (action === 'accept' && !form.slot) errors.slot = 'Выберите время';
  return errors;
}

export const hasErrors = (errors: object) => Object.values(errors).some(Boolean);

// ------------------------------------------------------------------ ошибки

const sentence = (text: string) => (text ? text.charAt(0).toUpperCase() + text.slice(1) : text);

/** Ошибка invite.create / list / cancel для владельца. */
export function ownerErrorText(error: unknown): string {
  if (!(error instanceof SelfvoyError)) return 'Что-то пошло не так';
  if (error.code === 'timeout') return 'Сервер не ответил вовремя — попробуйте ещё раз';
  if (isNetworkError(error)) return 'Нет связи с сервером Selfvoy';
  // Сервер присылает upstream_error (protocol.h, code::kUpstream), когда AI-сервис недоступен.
  if (error.code === 'upstream_error') return 'Selfvoy сейчас не может подобрать время — попробуйте позже';
  if (error.code === 'conflict' && !error.message) return 'Нет свободных окон в выбранный период';
  // forbidden — лимит открытых приглашений (текст от сервера).
  if (error.code === 'forbidden' && !error.message) return 'Слишком много открытых приглашений — отмените ненужные';
  return sentence(error.message) || 'Что-то пошло не так';
}

export type GuestErrorKind = 'network' | 'notFound' | 'conflict' | 'limited' | 'invalid' | 'other';

export function guestErrorKind(error: unknown): GuestErrorKind {
  if (!(error instanceof SelfvoyError)) return 'other';
  if (isNetworkError(error)) return 'network';
  switch (error.code) {
    case 'not_found':
      return 'notFound';
    case 'conflict':
      return 'conflict';
    case 'forbidden':
    case 'rate_limited':
      return 'limited';
    case 'bad_request':
      return 'invalid';
    default:
      return 'other';
  }
}

/** Текст ошибки для гостя: сетевые — своими словами, остальное — как сказал сервер. */
export function guestErrorText(error: unknown): string {
  const kind = guestErrorKind(error);
  if (kind === 'network') {
    return error instanceof SelfvoyError && error.code === 'timeout' ? 'Сервер не ответил вовремя — попробуйте ещё раз' : 'Нет связи с сервером Selfvoy — ответ не отправлен';
  }
  const message = error instanceof SelfvoyError ? sentence(error.message) : '';
  if (kind === 'limited') return message || 'Слишком много попыток — подождите минуту';
  // Сервер говорит, что именно не найдено (приглашение, страница или запись).
  if (kind === 'notFound') return message || 'Не найдено — проверьте ссылку';
  return message || 'Что-то пошло не так';
}
