import { describe, expect, it } from 'vitest';
import { SelfvoyError } from '../net/socket';
import {
  buildIcs,
  chosenSlot,
  dayKey,
  foldIcsLine,
  groupSlotsByDay,
  guestErrorKind,
  guestErrorText,
  guestFormErrors,
  guestIcs,
  hasErrors,
  icsEscape,
  icsFileName,
  icsUtc,
  inviteFormErrors,
  inviteLink,
  inviteStatus,
  inviteUid,
  isPlausibleToken,
  isPublicWebOrigin,
  ownerErrorText,
  slotPhrase,
  statusLabel,
  statusTone,
  timeZoneLabel,
} from './invite';

const TOKEN = 'AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_abcde';
const loc = (href: string) => {
  const u = new URL(href);
  return { protocol: u.protocol, hostname: u.hostname, origin: u.origin, pathname: u.pathname };
};

describe('inviteLink', () => {
  it('ссылка от сервера (SELFVOY_PUBLIC_URL) — как есть', () => {
    expect(inviteLink(TOKEN, 'https://selfvoy.example.com/#/i/' + TOKEN, loc('http://localhost:5173/'))).toEqual({
      kind: 'url',
      url: 'https://selfvoy.example.com/#/i/' + TOKEN,
    });
  });

  it('без url от сервера — адрес веб-клиента, если он публичный', () => {
    expect(inviteLink(TOKEN, '', loc('https://app.selfvoy.example/web/index.html'))).toEqual({
      kind: 'url',
      url: `https://app.selfvoy.example/web/index.html#/i/${TOKEN}`,
    });
    expect(inviteLink(TOKEN, '', loc('http://192.168.1.5:4173/'))).toEqual({ kind: 'url', url: `http://192.168.1.5:4173/#/i/${TOKEN}` });
  });

  it('localhost, Capacitor, Electron (file:) — только токен', () => {
    for (const href of ['http://localhost:5173/', 'https://localhost/', 'capacitor://localhost/', 'file:///app/dist/index.html', 'http://127.0.0.1:4173/', 'http://[::1]:5173/', 'http://selfvoy.localhost/']) {
      expect(inviteLink(TOKEN, '', loc(href))).toEqual({ kind: 'token', token: TOKEN });
    }
    expect(inviteLink(TOKEN, '', null)).toEqual({ kind: 'token', token: TOKEN });
  });

  it('не-http адрес от сервера не принимаем', () => {
    expect(inviteLink(TOKEN, 'javascript:alert(1)', null)).toEqual({ kind: 'token', token: TOKEN });
  });

  it('isPublicWebOrigin', () => {
    expect(isPublicWebOrigin(loc('https://selfvoy.example.com/'))).toBe(true);
    expect(isPublicWebOrigin(loc('https://localhost/'))).toBe(false);
    expect(isPublicWebOrigin(undefined)).toBe(false);
  });

  it('isPlausibleToken', () => {
    expect(isPlausibleToken(TOKEN)).toBe(true);
    expect(isPlausibleToken('')).toBe(false);
    expect(isPlausibleToken(undefined)).toBe(false);
    expect(isPlausibleToken('abc')).toBe(false);
    expect(isPlausibleToken('a'.repeat(20) + '/../')).toBe(false);
  });
});

describe('статусы', () => {
  const now = new Date('2026-10-14T12:00:00Z');

  it('подписи по-русски и тон бейджа', () => {
    expect(statusLabel('open')).toBe('Открыто');
    expect(statusLabel('accepted')).toBe('Время выбрано');
    expect(statusLabel('declined')).toBe('Отказ');
    expect(statusLabel('cancelled')).toBe('Отменено');
    expect(statusLabel('expired')).toBe('Истекло');
    expect(statusTone('accepted')).toBe('green');
    expect(statusTone('cancelled')).toBe('');
  });

  it('открытое с прошедшим сроком — истекло', () => {
    expect(inviteStatus({ status: 'open', expires_at: '2026-10-14T11:00:00Z' }, now)).toBe('expired');
    expect(inviteStatus({ status: 'open', expires_at: '2026-10-15T11:00:00Z' }, now)).toBe('open');
    expect(inviteStatus({ status: 'open', expires_at: '' }, now)).toBe('open');
    expect(inviteStatus({ status: 'accepted', expires_at: '2026-10-14T11:00:00Z' }, now)).toBe('accepted');
  });

  it('chosen: {} — времени нет', () => {
    expect(chosenSlot({ chosen: {} })).toBeNull();
    expect(chosenSlot({ chosen: { start: 'x', end: 'y' } })).toBeNull();
    expect(chosenSlot({ chosen: { start: '2026-10-15T07:00:00Z', end: '2026-10-15T08:00:00Z' } })).toEqual({
      start: '2026-10-15T07:00:00Z',
      end: '2026-10-15T08:00:00Z',
    });
  });
});

describe('groupSlotsByDay', () => {
  const now = new Date('2026-10-14T12:00:00Z'); // среда
  const slots = [
    { start: '2026-10-16T07:00:00Z', end: '2026-10-16T08:00:00Z' },
    { start: '2026-10-15T07:00:00Z', end: '2026-10-15T08:00:00Z' },
    { start: '2026-10-15T21:30:00Z', end: '2026-10-15T22:30:00Z' },
    { start: '2026-10-14T10:00:00Z', end: '2026-10-14T11:00:00Z' }, // уже прошёл
    { start: 'битый', end: '2026-10-15T08:00:00Z' },
  ];

  it('дни и время — в часовом поясе гостя (Москва)', () => {
    const groups = groupSlotsByDay(slots, 'Europe/Moscow', now);
    expect(groups.map((g) => g.key)).toEqual(['2026-10-15', '2026-10-16']);
    expect(groups[0].title).toBe('Завтра, 15 октября');
    // 21:30Z = 00:30 следующего дня по Москве
    expect(groups[1].title).toBe('Пятница, 16 октября');
    expect(groups[0].slots.map((s) => s.label)).toEqual(['10:00–11:00']);
    expect(groups[1].slots.map((s) => s.label)).toEqual(['00:30–01:30', '10:00–11:00']);
  });

  it('тот же набор в Нью-Йорке раскладывается иначе', () => {
    const groups = groupSlotsByDay(slots, 'America/New_York', now);
    expect(groups.map((g) => g.key)).toEqual(['2026-10-15', '2026-10-16']);
    expect(groups[0].slots.map((s) => s.label)).toEqual(['03:00–04:00', '17:30–18:30']);
    expect(groups[0].title).toBe('Завтра, 15 октября');
  });

  it('сегодня и прочее', () => {
    const groups = groupSlotsByDay([{ start: '2026-10-14T15:00:00Z', end: '2026-10-14T16:00:00Z' }], 'UTC', now);
    expect(groups[0].title).toBe('Сегодня, 14 октября');
    expect(groupSlotsByDay([], 'UTC', now)).toEqual([]);
  });

  it('неизвестный пояс не роняет страницу (UTC)', () => {
    expect(dayKey(new Date('2026-10-15T23:30:00Z'), 'Not/AZone')).toBe('2026-10-15');
  });

  it('slotPhrase и подпись пояса', () => {
    expect(slotPhrase({ start: '2026-10-17T07:00:00Z', end: '2026-10-17T08:00:00Z' }, 'Europe/Moscow', now)).toBe('Суббота, 17 октября, 10:00–11:00');
    expect(timeZoneLabel('Europe/Moscow', now)).toBe('Europe/Moscow (GMT+3)');
    expect(timeZoneLabel('America/New_York', now)).toBe('America/New York (GMT-4)');
  });
});

describe('.ics', () => {
  it('UTC-время и экранирование TEXT', () => {
    expect(icsUtc(new Date('2026-10-15T07:00:00.000Z'))).toBe('20261015T070000Z');
    expect(icsEscape('a,b;c\\d\nновая')).toBe('a\\,b\\;c\\\\d\\nновая');
    expect(icsEscape('x\r\ny')).toBe('x\\ny');
  });

  it('VCALENDAR/VEVENT с CRLF', () => {
    const ics = buildIcs({
      uid: 'u1@selfvoy',
      start: '2026-10-15T10:00:00+03:00',
      end: '2026-10-15T11:00:00+03:00',
      summary: 'Кофе, наконец; да',
      description: 'Строка 1\nСтрока 2',
      now: new Date('2026-10-14T12:00:00Z'),
    });
    expect(ics.endsWith('\r\n')).toBe(true);
    expect(ics.replace(/\r\n/g, '').includes('\n')).toBe(false);
    const lines = ics.split('\r\n');
    expect(lines[0]).toBe('BEGIN:VCALENDAR');
    expect(lines).toContain('VERSION:2.0');
    expect(lines).toContain('BEGIN:VEVENT');
    expect(lines).toContain('DTSTART:20261015T070000Z');
    expect(lines).toContain('DTEND:20261015T080000Z');
    expect(lines).toContain('DTSTAMP:20261014T120000Z');
    expect(lines).toContain('SUMMARY:Кофе\\, наконец\\; да');
    expect(lines).toContain('DESCRIPTION:Строка 1\\nСтрока 2');
    expect(lines.at(-2)).toBe('END:VCALENDAR');
  });

  it('битое время — ошибка, а не пустой файл', () => {
    expect(() => buildIcs({ uid: 'x', start: 'nope', end: 'nope', summary: 's' })).toThrow();
  });

  it('длинные строки переносятся по 75 октетов без разрыва символов', () => {
    const line = 'SUMMARY:' + 'Встреча '.repeat(30);
    const folded = foldIcsLine(line);
    const parts = folded.split('\r\n');
    expect(parts.length).toBeGreaterThan(1);
    const enc = new TextEncoder();
    for (const p of parts) expect(enc.encode(p).length).toBeLessThanOrEqual(75);
    for (const p of parts.slice(1)) expect(p.startsWith(' ')).toBe(true);
    expect(parts.map((p, i) => (i ? p.slice(1) : p)).join('')).toBe(line);
    expect(foldIcsLine('short')).toBe('short');
  });

  it('событие гостя: тема и владелец, без токена; стабильный UID', () => {
    const slot = { start: '2026-10-15T07:00:00Z', end: '2026-10-15T08:00:00Z' };
    const ics = guestIcs({ topic: 'Кофе', note: 'У входа', owner_name: 'Иван' }, slot, new Date('2026-10-14T12:00:00Z'));
    expect(ics).toContain('SUMMARY:Кофе — Иван');
    const unfolded = ics.replace(/\r\n /g, '');
    expect(unfolded).toContain('DESCRIPTION:У входа\\n\\nОрганизатор: Иван. Приглашение через Selfvoy');
    expect(inviteUid('Кофе', 'Иван', slot.start)).toBe(inviteUid('Кофе', 'Иван', slot.start));
    expect(inviteUid('Кофе', 'Иван', slot.start)).toMatch(/^invite-20261015T070000Z-[0-9a-f]{8}@selfvoy$/);
    expect(icsFileName(slot)).toBe('selfvoy-invite-20261015.ics');
  });
});

describe('формы', () => {
  it('владелец: тема обязательна, ограничения длины', () => {
    expect(inviteFormErrors({ topic: '  ', note: '', guestName: '' }).topic).toBe('Укажите тему встречи');
    expect(inviteFormErrors({ topic: 'x'.repeat(121), note: '', guestName: '' }).topic).toBeTruthy();
    expect(inviteFormErrors({ topic: 'Кофе', note: 'x'.repeat(501), guestName: 'y'.repeat(81) })).toMatchObject({ note: expect.any(String), guestName: expect.any(String) });
    expect(hasErrors(inviteFormErrors({ topic: 'Кофе', note: '', guestName: '' }))).toBe(false);
  });

  it('гость: имя обязательно, email и комментарий — по желанию, для accept нужен слот', () => {
    expect(guestFormErrors({ name: '', email: '', comment: '', slot: 's' }, 'accept').name).toBeTruthy();
    expect(guestFormErrors({ name: 'Аня', email: '', comment: '' }, 'accept').slot).toBe('Выберите время');
    expect(hasErrors(guestFormErrors({ name: 'Аня', email: '', comment: '' }, 'decline'))).toBe(false);
    expect(guestFormErrors({ name: 'Аня', email: 'anna@', comment: '', slot: 's' }, 'accept').email).toBeTruthy();
    expect(guestFormErrors({ name: 'Аня', email: 'a'.repeat(250) + '@x.ru', comment: '', slot: 's' }, 'accept').email).toBeTruthy();
    expect(guestFormErrors({ name: 'Аня', email: '', comment: 'x'.repeat(501), slot: 's' }, 'accept').comment).toBeTruthy();
    expect(hasErrors(guestFormErrors({ name: 'Аня', email: 'anna@example.com', comment: 'ок', slot: 's' }, 'accept'))).toBe(false);
  });
});

describe('ошибки', () => {
  it('владелец', () => {
    expect(ownerErrorText(new SelfvoyError('offline', ''))).toBe('Нет связи с сервером Selfvoy');
    expect(ownerErrorText(new SelfvoyError('upstream_error', 'не удалось подобрать время'))).toBe('Selfvoy сейчас не может подобрать время — попробуйте позже');
    expect(ownerErrorText(new SelfvoyError('conflict', 'нет свободных окон в выбранный период'))).toBe('Нет свободных окон в выбранный период');
    expect(ownerErrorText(new SelfvoyError('forbidden', 'не больше 20 открытых приглашений'))).toBe('Не больше 20 открытых приглашений');
    expect(ownerErrorText(new Error('x'))).toBe('Что-то пошло не так');
  });

  it('владелец: код upstream_error (как шлёт сервер), а не upstream', () => {
    // Чужой код — обычная ошибка с текстом сервера, без подмены.
    expect(ownerErrorText(new SelfvoyError('upstream', 'ai down'))).toBe('Ai down');
    expect(ownerErrorText(new SelfvoyError('upstream_error', ''))).toContain('Selfvoy сейчас не может');
  });

  it('гость', () => {
    expect(guestErrorKind(new SelfvoyError('disconnected', ''))).toBe('network');
    expect(guestErrorKind(new SelfvoyError('not_found', 'приглашение не найдено'))).toBe('notFound');
    expect(guestErrorKind(new SelfvoyError('conflict', 'это время уже занято — выберите другое'))).toBe('conflict');
    expect(guestErrorKind(new SelfvoyError('forbidden', 'слишком много запросов'))).toBe('limited');
    expect(guestErrorKind(new SelfvoyError('bad_request', 'плохой email'))).toBe('invalid');
    expect(guestErrorText(new SelfvoyError('forbidden', 'слишком много запросов'))).toBe('Слишком много запросов');
    expect(guestErrorText(new SelfvoyError('offline', ''))).toContain('Нет связи');
    expect(guestErrorText(new SelfvoyError('bad_request', 'слот не из списка'))).toBe('Слот не из списка');
  });
});
