import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SelfvoyError } from '../net/socket';
import type { Occurrence } from '../net/bookingApi';
import {
  bookingIcs,
  bookingOwnerErrorText,
  bookingStatus,
  bookingUid,
  changedFields,
  forgetRef,
  guestActions,
  isPlausibleSlug,
  nextOccurrence,
  occurrenceAt,
  PAGE_DEFAULTS,
  pageFields,
  pageFormErrors,
  parseManageQuery,
  parseRef,
  publicLink,
  recalledRef,
  rememberRef,
  seriesLabel,
  signupPath,
  sortedOccurrences,
  upcomingCount,
  withPostLink,
} from './booking';

const loc = (href: string) => {
  const u = new URL(href);
  return { protocol: u.protocol, hostname: u.hostname, origin: u.origin, pathname: u.pathname };
};
const NOW = new Date('2026-10-14T12:00:00Z');
const occ = (start: string, status: Occurrence['status'] = 'active'): Occurrence => ({
  start,
  end: new Date(Date.parse(start) + 3_600_000).toISOString(),
  status,
});

describe('ссылки', () => {
  it('slug — 10 символов [a-z0-9]', () => {
    expect(isPlausibleSlug('abc123def4')).toBe(true);
    for (const bad of [undefined, '', 'ABC123DEF4', 'abc123def', 'abc123def45', '../etc/pas']) expect(isPlausibleSlug(bad)).toBe(false);
  });

  it('публичная ссылка: от сервера, иначе публичный адрес клиента, иначе null', () => {
    expect(publicLink('/b/abc123def4', 'https://selfvoy.example.com/#/b/abc123def4', loc('http://localhost:5173/'))).toBe('https://selfvoy.example.com/#/b/abc123def4');
    expect(publicLink('/b/abc123def4', '', loc('https://app.example/web/'))).toBe('https://app.example/web/#/b/abc123def4');
    expect(publicLink('/m/tok', '', loc('http://localhost:5173/'))).toBeNull();
    expect(publicLink('/m/tok', undefined, loc('http://localhost:5173/'))).toBeNull();
    expect(publicLink('/m/tok', null, loc('http://localhost:5173/'))).toBeNull();
    expect(publicLink('/m/tok', 'javascript:alert(1)', null)).toBeNull();
  });

  it('путь регистрации с кодом и источником', () => {
    expect(signupPath('ABCD2345')).toBe('/signup?ref=ABCD2345');
    expect(signupPath('ABCD2345', 'booking')).toBe('/signup?ref=ABCD2345&src=booking');
    expect(signupPath('', 'booking')).toBe('/signup');
  });
});

describe('реферальный код', () => {
  beforeEach(() => {
    const store = new Map<string, string>();
    vi.stubGlobal('sessionStorage', {
      getItem: (k: string) => store.get(k) ?? null,
      setItem: (k: string, v: string) => void store.set(k, v),
      removeItem: (k: string) => void store.delete(k),
    });
  });
  afterEach(() => {
    forgetRef();
    vi.unstubAllGlobals();
  });

  it('разбор: 8 букв/цифр, регистр не важен, мусор — null', () => {
    expect(parseRef(new URLSearchParams('ref=abcd2345&src=booking'))).toEqual({ code: 'ABCD2345', source: 'booking' });
    expect(parseRef(new URLSearchParams('ref=ABCD2345&src=evil'))).toEqual({ code: 'ABCD2345', source: 'link' });
    expect(parseRef(new URLSearchParams('ref=<script>'))).toBeNull();
    expect(parseRef(new URLSearchParams(''))).toBeNull();
  });

  it('переживает переход внутри вкладки', () => {
    expect(recalledRef()).toBeNull();
    rememberRef({ code: 'ABCD2345', source: 'invite' });
    expect(recalledRef()).toEqual({ code: 'ABCD2345', source: 'invite' });
    forgetRef();
    expect(recalledRef()).toBeNull();
  });

  it('без хранилища (приватный режим) — молча без кода', () => {
    vi.stubGlobal('sessionStorage', undefined);
    rememberRef({ code: 'ABCD2345', source: 'link' });
    expect(recalledRef()).toBeNull();
  });
});

describe('форма страницы', () => {
  it('по умолчанию ошибка только в названии', () => {
    expect(pageFormErrors(PAGE_DEFAULTS)).toEqual({ title: expect.any(String) });
    expect(pageFormErrors({ ...PAGE_DEFAULTS, title: 'Консультация' })).toEqual({});
  });

  it('пределы как на сервере', () => {
    const ok = { ...PAGE_DEFAULTS, title: 'Встреча' };
    expect(pageFormErrors({ ...ok, title: 'а'.repeat(121) }).title).toBeTruthy();
    expect(pageFormErrors({ ...ok, title: 'две\nстроки' }).title).toBeTruthy();
    expect(pageFormErrors({ ...ok, description: 'а'.repeat(1001) }).description).toBeTruthy();
    for (const bad of [{ duration_minutes: 14 }, { days_ahead: 61 }, { max_per_day: 0 }, { notice_hours: 169 }, { max_series: 13 }, { max_per_day: Number.NaN }, { max_series: 1.5 }]) {
      expect(pageFormErrors({ ...ok, ...bad }).numbers).toBeTruthy();
    }
    expect(pageFormErrors({ ...ok, notice_hours: 0, max_series: 12, days_ahead: 60, max_per_day: 16, duration_minutes: 240 })).toEqual({});
  });
});

describe('встречи', () => {
  const list = [occ('2026-10-28T07:00:00Z'), occ('2026-10-14T07:00:00Z'), occ('2026-10-21T07:00:00Z', 'cancelled'), occ('2026-10-15T07:00:00Z')];

  it('порядок, ближайшая и число предстоящих', () => {
    expect(sortedOccurrences(list).map((o) => o.start.slice(0, 10))).toEqual(['2026-10-14', '2026-10-15', '2026-10-21', '2026-10-28']);
    expect(nextOccurrence(list, NOW)?.start).toBe('2026-10-15T07:00:00Z');
    expect(upcomingCount(list, NOW)).toBe(2);
    expect(nextOccurrence([], NOW)).toBeNull();
  });

  it('статус записи для владельца', () => {
    expect(bookingStatus({ status: 'active', cancelled_by: '', occurrences: list }, NOW)).toEqual({ label: 'Предстоит', tone: 'green' });
    expect(bookingStatus({ status: 'active', cancelled_by: '', occurrences: [occ('2026-10-01T07:00:00Z')] }, NOW).label).toBe('Прошла');
    expect(bookingStatus({ status: 'cancelled', cancelled_by: 'owner', occurrences: [] }, NOW).label).toBe('Отменили вы');
    expect(bookingStatus({ status: 'cancelled', cancelled_by: 'guest', occurrences: [] }, NOW).label).toBe('Отменил гость');
  });

  it('подпись серии', () => {
    expect(seriesLabel(1)).toBe('Одна встреча');
    expect(seriesLabel(2)).toBe('Серия: 2 встречи, раз в неделю');
    expect(seriesLabel(5)).toBe('Серия: 5 встреч, раз в неделю');
  });
});

const TOKEN = 'a'.repeat(43);

describe('bookingIcs', () => {
  it('одно событие на каждую активную встречу, CRLF', () => {
    const ics = bookingIcs({ page_title: 'Консультация', owner_name: 'Ольга', occurrences: [occ('2026-10-21T07:00:00Z'), occ('2026-10-28T07:00:00Z', 'cancelled'), occ('2026-11-04T07:00:00Z')] }, TOKEN, NOW);
    expect(ics.match(/BEGIN:VEVENT/g)).toHaveLength(2);
    expect(ics).toContain('DTSTART:20261021T070000Z');
    expect(ics).toContain('DTSTART:20261104T070000Z');
    expect(ics).not.toContain('DTSTART:20261028T070000Z');
    expect(ics).toContain('SUMMARY:Консультация — Ольга');
    expect(ics.endsWith('END:VCALENDAR\r\n')).toBe(true);
    expect(ics.split('\r\n').every((line) => !line.includes('\n'))).toBe(true);
  });

  it('без активных встреч — ошибка', () => {
    expect(() => bookingIcs({ page_title: 'X', owner_name: '', occurrences: [occ('2026-10-21T07:00:00Z', 'cancelled')] }, TOKEN)).toThrow();
  });

  it('UID встречи не меняется после переноса (без дубля в календаре), SEQUENCE растёт', () => {
    const uids = (ics: string) => ics.split('\r\n').filter((l) => l.startsWith('UID:'));
    const before = bookingIcs(
      { page_title: 'Консультация', owner_name: 'Ольга', reschedules_left: 3, occurrences: [occ('2026-10-21T07:00:00Z'), occ('2026-10-28T07:00:00Z')] },
      TOKEN,
      NOW,
    );
    // Первую встречу перенесли на после второй: сервер оставляет её на том же месте в списке.
    const after = bookingIcs(
      { page_title: 'Консультация', owner_name: 'Ольга', reschedules_left: 2, occurrences: [occ('2026-11-02T09:00:00Z'), occ('2026-10-28T07:00:00Z')] },
      TOKEN,
      NOW,
    );
    expect(uids(before)).toEqual([`UID:${bookingUid(TOKEN, 0)}`, `UID:${bookingUid(TOKEN, 1)}`]);
    // В файле события по времени, но UID каждой встречи прежний.
    expect(uids(after)).toEqual([`UID:${bookingUid(TOKEN, 1)}`, `UID:${bookingUid(TOKEN, 0)}`]);
    expect(before).toContain('SEQUENCE:0');
    expect(after).toContain('SEQUENCE:1');
  });

  it('UID: без времени и токена, разный у разных записей и встреч', () => {
    const uid = bookingUid(TOKEN, 0);
    expect(uid).toMatch(/^booking-[0-9a-f]{16}-0@selfvoy$/);
    expect(uid).not.toContain(TOKEN);
    expect(bookingUid(TOKEN, 0)).toBe(uid);
    expect(bookingUid(TOKEN, 1)).not.toBe(uid);
    expect(bookingUid('b'.repeat(43), 0)).not.toBe(uid);
  });
});

describe('guestActions', () => {
  const base = { can_change: false, reschedules_left: 3 };
  it('страница на паузе: отменить можно, перенести нельзя', () => {
    expect(guestActions({ ...base, can_cancel: true, can_reschedule: false })).toEqual({ canCancel: true, canReschedule: false });
  });
  it('всё открыто', () => {
    expect(guestActions({ ...base, can_change: true, can_cancel: true, can_reschedule: true })).toEqual({ canCancel: true, canReschedule: true });
  });
  it('переносы кончились — только отмена', () => {
    expect(guestActions({ can_change: true, can_cancel: true, can_reschedule: true, reschedules_left: 0 })).toEqual({ canCancel: true, canReschedule: false });
  });
  it('старый сервер без новых полей — по can_change', () => {
    expect(guestActions({ ...base, can_change: true })).toEqual({ canCancel: true, canReschedule: true });
    expect(guestActions({ ...base, can_change: false })).toEqual({ canCancel: false, canReschedule: false });
    expect(guestActions({ can_change: true, reschedules_left: 0 })).toEqual({ canCancel: true, canReschedule: false });
  });
  it('запись отменена — ничего', () => {
    expect(guestActions({ ...base, can_cancel: false, can_reschedule: false })).toEqual({ canCancel: false, canReschedule: false });
  });
});

describe('ошибки владельца', () => {
  it('сеть и таймаут — своими словами, остальное — текст сервера', () => {
    expect(bookingOwnerErrorText(new SelfvoyError('timeout', ''))).toMatch(/не ответил/);
    expect(bookingOwnerErrorText(new SelfvoyError('forbidden', 'не больше 10 страниц записи'))).toBe('Не больше 10 страниц записи');
    expect(bookingOwnerErrorText(new Error('x'))).toBe('Что-то пошло не так');
  });
});

describe('цена, предоплата и политика (F4)', () => {
  const ok = { ...PAGE_DEFAULTS, title: 'Урок' };

  it('по умолчанию — без цены и предоплаты, ошибок нет', () => {
    expect(pageFormErrors(ok)).toEqual({});
    expect(ok).toMatchObject({ price: 0, currency: 'RUB', prepay: 'none', pay_within_minutes: 60, cancel_hours: 0, late_cancel: 'allow', guest_reminders: true, hide_branding: false });
  });

  it('предоплата требует цену и способ оплаты', () => {
    expect(pageFormErrors({ ...ok, prepay: 'full' }).price).toBe('Для предоплаты укажите цену');
    expect(pageFormErrors({ ...ok, prepay: 'full', price: 200000 }).payment).toMatch(/как платить/);
    expect(pageFormErrors({ ...ok, prepay: 'full', price: 200000 }, { yookassa: true })).toEqual({});
    expect(pageFormErrors({ ...ok, prepay: 'full', price: 200000, payment_note: 'СБП +7 900 000-00-00' })).toEqual({});
    expect(pageFormErrors({ ...ok, prepay: 'deposit', price: 200000, deposit: 0, payment_url: 'https://pay.example/a' }).price).toMatch(/Депозит/);
    expect(pageFormErrors({ ...ok, prepay: 'deposit', price: 200000, deposit: 300000, payment_url: 'https://pay.example/a' }).price).toMatch(/Депозит/);
    expect(pageFormErrors({ ...ok, prepay: 'deposit', price: 200000, deposit: 50000, payment_url: 'https://pay.example/a' })).toEqual({});
  });

  it('пределы полей — как на сервере', () => {
    expect(pageFormErrors({ ...ok, price: 100_000_001 }).price).toBeTruthy();
    expect(pageFormErrors({ ...ok, payment_url: 'http://pay.example' }).payment).toMatch(/https/);
    expect(pageFormErrors({ ...ok, payment_url: 'https://pay example' }).payment).toMatch(/https/);
    expect(pageFormErrors({ ...ok, payment_note: 'две\nстроки' }).payment).toMatch(/одна строка/);
    expect(pageFormErrors({ ...ok, pay_within_minutes: 10 }).payment).toMatch(/Срок оплаты/);
    expect(pageFormErrors({ ...ok, cancel_hours: 169 }).policy).toBeTruthy();
    expect(pageFormErrors({ ...ok, policy_text: 'а'.repeat(501) }).policy).toBeTruthy();
  });

  it('форма страницы старого сервера получает значения по умолчанию', () => {
    const page = { id: 1, slug: 'abc123def4', url: '', title: 'Урок', description: '', duration_minutes: 60, days_ahead: 14, max_per_day: 4, notice_hours: 2, max_series: 1, weekends: false, status: 'active' as const, upcoming: 0, created_at: '', updated_at: '' };
    expect(pageFields(page)).toMatchObject({ price: 0, prepay: 'none', guest_reminders: true, hide_branding: false });
    expect(pageFields(page).packages).toEqual([]);
    expect(pageFormErrors({ ...pageFields(page), packages: [{ title: '', sessions: 8, price: 100 }] }).packages).toMatch(/Пакет 1/);
    expect(pageFields({ ...page, price: 250000, prepay: 'full', hide_branding: true })).toMatchObject({ price: 250000, prepay: 'full', hide_branding: true });
  });

  it('в pageUpdate уходят только изменённые поля', () => {
    expect(changedFields(ok, { ...ok, price: 200000, guest_reminders: false })).toEqual({ price: 200000, guest_reminders: false });
    expect(changedFields(ok, ok)).toEqual({});
    // Пакеты (F5): по содержимому, целым массивом, id сохранённых — на месте.
    const offer = { id: 1, title: '8 занятий', sessions: 8, price: 1200000, valid_days: 90 };
    const withOffer = { ...ok, packages: [offer] };
    expect(changedFields(withOffer, { ...withOffer, packages: [{ ...offer }] })).toEqual({});
    expect(changedFields(withOffer, { ...withOffer, packages: [{ ...offer, price: 1000000 }, { title: ' Месяц ', sessions: 4, price: 500000 }] })).toEqual({
      packages: [{ ...offer, price: 1000000 }, { title: 'Месяц', sessions: 4, price: 500000, valid_days: 90 }],
    });
    expect(changedFields(withOffer, { ...withOffer, packages: [] })).toEqual({ packages: [] });
  });

  it('pro_required — понятный текст', () => {
    expect(bookingOwnerErrorText(new SelfvoyError('pro_required', 'нужен Pro'))).toMatch(/тарифе Pro/);
  });

  it('отмена системой — «нет оплаты»', () => {
    expect(bookingStatus({ status: 'cancelled', cancelled_by: 'system', occurrences: [] }, NOW).label).toBe('Отменена: нет оплаты');
  });
});

describe('ссылка управления: ?confirm= и ?paid=1', () => {
  it('разбирает «Приду» из напоминания и возврат после оплаты', () => {
    expect(parseManageQuery('?confirm=2026-10-21T12:00:00Z')).toEqual({ confirm: '2026-10-21T12:00:00Z', paid: false });
    expect(parseManageQuery('confirm=2026-10-21T15%3A00%3A00%2B03%3A00&paid=1')).toEqual({ confirm: '2026-10-21T15:00:00+03:00', paid: true });
    expect(parseManageQuery(new URLSearchParams({ paid: '1' }))).toEqual({ confirm: null, paid: true });
    expect(parseManageQuery('')).toEqual({ confirm: null, paid: false });
  });

  it('мусор в confirm не отправляется', () => {
    expect(parseManageQuery('?confirm=завтра').confirm).toBeNull();
    expect(parseManageQuery(`?confirm=${'1'.repeat(60)}`).confirm).toBeNull();
    expect(parseManageQuery('?paid=yes').paid).toBe(false);
  });

  it('встреча по времени из ссылки — и в другой записи пояса', () => {
    const list = [occ('2026-10-21T12:00:00Z'), occ('2026-10-28T12:00:00Z')];
    expect(occurrenceAt(list, '2026-10-28T12:00:00Z')?.start).toBe('2026-10-28T12:00:00Z');
    expect(occurrenceAt(list, '2026-10-21T15:00:00+03:00')?.start).toBe('2026-10-21T12:00:00Z');
    expect(occurrenceAt(list, '2026-10-22T12:00:00Z')).toBeNull();
  });
});

describe('пост «свободные окна»: ссылка без SELFVOY_PUBLIC_URL', () => {
  const text = 'Свободные окна: Урок (60 мин)\n\nПн, 21 окт: 10:00, 11:30\n';
  const link = 'https://app.example/#/b/abc123def4';
  it('сервер дал ссылку — текст как есть', () => {
    expect(withPostLink(text, 'telegram', link, link)).toBe(text);
  });
  it('без ссылки сервера — добавляем в формате поста', () => {
    expect(withPostLink(text, 'telegram', '', link)).toBe(`Свободные окна: Урок (60 мин)\n\nПн, 21 окт: 10:00, 11:30\n\nЗаписаться: ${link}`);
    expect(withPostLink(text, 'vk', '', link)).toMatch(/\n\nЗапись онлайн: https:/);
    expect(withPostLink('a\nb', 'plain', '', link)).toBe(`a\nb\nЗаписаться: ${link}`);
    expect(withPostLink(text, 'plain', '', null)).toBe(text);
  });
});
