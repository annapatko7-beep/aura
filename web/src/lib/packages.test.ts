import { describe, expect, it } from 'vitest';
import {
  canBookByPackage,
  ccEmailError,
  cleanOffers,
  issueErrors,
  leftLabel,
  newOffer,
  offerSummary,
  offersEqual,
  offersError,
  packageBlockReason,
  packageBookPath,
  packageNoticeDetail,
  packagePath,
  packageSessionNote,
  packageProgress,
  packageStatusView,
  packageTokenFrom,
  perSessionLabel,
  ringDash,
  sessionsLabel,
  validityLabel,
  type IssueForm,
} from './packages';

const NBSP = ' ';
const TOKEN = 'pAcKaGeToKeN_abcdefghijklmnopqrstuvwxyz0123';

describe('ссылки пакета', () => {
  it('путь пакета и запись по абонементу', () => {
    expect(packagePath(TOKEN)).toBe(`/p/${TOKEN}`);
    expect(packageBookPath('anna2026ab', TOKEN)).toBe(`/b/anna2026ab?pkg=${TOKEN}`);
  });
  it('токен из адреса: только base64url разумной длины', () => {
    expect(packageTokenFrom(`?pkg=${TOKEN}`)).toBe(TOKEN);
    expect(packageTokenFrom(new URLSearchParams({ pkg: TOKEN }))).toBe(TOKEN);
    expect(packageTokenFrom('?pkg=short')).toBeNull();
    expect(packageTokenFrom('?pkg=<script>alert(1)</script>xxxxxxxxxxxx')).toBeNull();
    expect(packageTokenFrom('')).toBeNull();
  });
});

describe('подписи', () => {
  it('статус — значок и слово', () => {
    expect(packageStatusView('active')).toEqual({ label: 'Действует', tone: 'green', icon: 'check' });
    expect(packageStatusView('pending', 'guest')).toEqual({ label: 'Ждём оплату', tone: 'amber', icon: 'clock' });
    expect(packageStatusView('cancelled').icon).toBe('x');
    expect(packageStatusView('weird').label).toBe('weird');
  });
  it('занятия, цена за занятие, сводка предложения', () => {
    expect(sessionsLabel(1)).toBe('1 занятие');
    expect(sessionsLabel(3)).toBe('3 занятия');
    expect(sessionsLabel(8)).toBe('8 занятий');
    expect(perSessionLabel(1200000, 8)).toBe(`1${NBSP}500${NBSP}₽ за занятие`);
    expect(perSessionLabel(0, 8)).toBe('');
    expect(offerSummary({ sessions: 8, price: 1200000, valid_days: 60 })).toBe(`8 занятий · 12${NBSP}000${NBSP}₽ · 60 дней`);
    expect(offerSummary({ sessions: 4, price: 500000 })).toBe(`4 занятия · 5${NBSP}000${NBSP}₽ · 90 дней`);
  });
  it('остаток и прогресс', () => {
    expect(packageProgress({ sessions_total: 8, sessions_left: 3, sessions_used: 5 })).toEqual({ used: 5, total: 8, left: 3, ratio: 5 / 8 });
    expect(packageProgress({ sessions_total: 8, sessions_left: 3 })).toMatchObject({ used: 5 });
    expect(packageProgress({ sessions_total: 0, sessions_left: 0 }).ratio).toBe(0);
    expect(packageProgress({ sessions_total: 4, sessions_left: 9 }).left).toBe(4);
    expect(leftLabel({ sessions_total: 8, sessions_left: 3 })).toBe('осталось 3 из 8');
  });
  it('дуга кольца', () => {
    const full = ringDash(8, 8, 10);
    expect(full.offset).toBeCloseTo(0);
    expect(full.length).toBeCloseTo(2 * Math.PI * 10);
    expect(ringDash(0, 8, 10).offset).toBeCloseTo(full.length);
    expect(ringDash(4, 8, 10).offset).toBeCloseTo(full.length / 2);
    expect(ringDash(1, 0, 10).offset).toBeCloseTo(full.length);
  });
  it('срок действия', () => {
    const now = new Date('2026-10-22T12:00:00Z');
    expect(validityLabel({ valid_until: '', valid_days: 90 }, now)).toBe('90 дней с момента оплаты');
    expect(validityLabel({ valid_until: '2026-11-01T12:00:00Z', valid_days: 90, status: 'active' }, now)).toBe('Срок — до 1 ноября · ещё 10 дней');
    expect(validityLabel({ valid_until: '2027-01-20T12:00:00Z', valid_days: 90, status: 'active' }, now)).toContain('2027');
    expect(validityLabel({ valid_until: '2026-10-01T12:00:00Z', valid_days: 90, status: 'expired' }, now)).toMatch(/^Срок истёк/);
  });
});

describe('запись по пакету', () => {
  const now = new Date('2026-10-22T12:00:00Z');
  const ok = { status: 'active', sessions_left: 3, page_slug: 'anna2026ab', valid_until: '2026-12-01T00:00:00Z' };
  it('можно — действует, есть занятия, страница открыта, срок не истёк', () => {
    expect(canBookByPackage(ok, now)).toBe(true);
    expect(packageBlockReason(ok, now)).toBe('');
  });
  it('нельзя — с понятной причиной', () => {
    for (const [pkg, text] of [
      [{ ...ok, status: 'pending' }, /после оплаты/],
      [{ ...ok, status: 'cancelled' }, /отменён/],
      [{ ...ok, valid_until: '2026-10-01T00:00:00Z' }, /истёк/],
      [{ ...ok, status: 'used', sessions_left: 0 }, /закончились/],
      [{ ...ok, page_slug: '' }, /закрыта/],
    ] as const) {
      expect(canBookByPackage(pkg, now)).toBe(false);
      expect(packageBlockReason(pkg, now)).toMatch(text);
    }
  });
});

describe('предложения страницы', () => {
  const offer = { id: 1, title: '8 занятий', sessions: 8, price: 1200000, valid_days: 90 };
  it('пределы как на сервере', () => {
    expect(offersError([])).toBeNull();
    expect(offersError(undefined)).toBeNull();
    expect(offersError([offer])).toBeNull();
    expect(offersError(Array.from({ length: 6 }, () => offer))).toMatchObject({ index: -1 });
    expect(offersError([offer, { ...offer, title: '  ' }])).toMatchObject({ index: 1 });
    expect(offersError([{ ...offer, title: 'а\nб' }])?.text).toMatch(/одну строку/);
    expect(offersError([{ ...offer, title: 'я'.repeat(81) }])?.text).toMatch(/80/);
    expect(offersError([{ ...offer, sessions: 1 }])?.text).toMatch(/от 2 до 100/);
    expect(offersError([{ ...offer, sessions: Number.NaN }])?.text).toMatch(/от 2 до 100/);
    expect(offersError([{ ...offer, price: 0 }])?.text).toMatch(/цену/);
    expect(offersError([{ ...offer, price: 100_000_001 }])?.text).toMatch(/цену/);
    expect(offersError([{ ...offer, valid_days: 6 }])?.text).toMatch(/7 до 365/);
    expect(offersError([{ ...offer, valid_days: undefined }])).toBeNull();
  });
  it('для сервера: id только у сохранённых, названия без пробелов, срок по умолчанию', () => {
    expect(cleanOffers([{ ...offer, title: '  8 занятий ' }, { ...newOffer(), title: 'Новый', price: 500000 }, { id: 0, title: 'x', sessions: 2, price: 1 }])).toEqual([
      { id: 1, title: '8 занятий', sessions: 8, price: 1200000, valid_days: 90 },
      { title: 'Новый', sessions: 8, price: 500000, valid_days: 90 },
      { title: 'x', sessions: 2, price: 1, valid_days: 90 },
    ]);
  });
  it('сравнение по содержимому', () => {
    expect(offersEqual([offer], [{ ...offer }])).toBe(true);
    expect(offersEqual([offer], [{ ...offer, title: '8 занятий ' }])).toBe(true);
    expect(offersEqual([offer], [{ ...offer, price: 1 }])).toBe(false);
    expect(offersEqual([offer], [])).toBe(false);
    expect(offersEqual(undefined, [])).toBe(true);
  });
});

describe('второй получатель', () => {
  it('необязателен, формат email, не совпадает с адресом гостя', () => {
    expect(ccEmailError('', 'a@b.ru')).toBeNull();
    expect(ccEmailError('  ', '')).toBeNull();
    expect(ccEmailError('mama@example.com', 'kid@example.com')).toBeNull();
    expect(ccEmailError('mama@example.com', '')).toBeNull();
    expect(ccEmailError('mama@', 'kid@example.com')).toMatch(/опечатка/);
    expect(ccEmailError(' Kid@Example.com ', 'kid@example.com')).toMatch(/ваш же адрес/);
  });
});

describe('выдача владельцем', () => {
  const form: IssueForm = { page_id: 1, offer_id: 3, title: '', sessions: 8, price: 0, valid_days: 90, guest_name: 'Маша', guest_email: '', cc_email: '', paid: false };
  it('готовое предложение — нужно только имя', () => {
    expect(issueErrors(form)).toEqual({});
    expect(issueErrors({ ...form, guest_name: ' ' })).toEqual({ guest_name: expect.any(String) });
    expect(issueErrors({ ...form, page_id: 0 })).toEqual({ page: expect.any(String) });
  });
  it('своё предложение — название, 1–100 занятий, цена ≥ 0, срок 7–365', () => {
    const own = { ...form, offer_id: 0, title: 'Месяц', sessions: 1 };
    expect(issueErrors(own)).toEqual({});
    expect(issueErrors({ ...own, title: '' }).offer).toBeTruthy();
    expect(issueErrors({ ...own, sessions: 101 }).offer).toMatch(/1 до 100/);
    expect(issueErrors({ ...own, price: Number.NaN }).offer).toMatch(/цену/);
    expect(issueErrors({ ...own, valid_days: 400 }).offer).toMatch(/7 до 365/);
  });
  it('email и копия', () => {
    expect(issueErrors({ ...form, guest_email: 'bad' }).guest_email).toBeTruthy();
    expect(issueErrors({ ...form, guest_email: 'a@b.ru', cc_email: 'A@B.ru' }).cc_email).toBeTruthy();
    expect(issueErrors({ ...form, guest_email: 'a@b.ru', cc_email: 'mama@b.ru' })).toEqual({});
  });
});

describe('уведомления о пакетах', () => {
  it('строка из payload', () => {
    expect(packageNoticeDetail('package.purchased', { guest_name: 'Маша' })).toBe('Маша: купил(а) пакет при записи');
    expect(packageNoticeDetail('package.low', { guest_name: 'Маша', left: 1 })).toBe('Маша: осталось 1 занятие — предложите продлить');
    expect(packageNoticeDetail('package.low', { left: 0 })).toBe('занятия по абонементу закончились');
    expect(packageNoticeDetail('package.payment', { guest_name: 'Маша', status: 'claimed', amount: 1200000, currency: 'RUB' })).toBe(`Маша: отметил(а) оплату 12${NBSP}000${NBSP}₽ — проверьте поступление`);
    expect(packageNoticeDetail('package.payment', { status: 'expired' })).toMatch(/не поступила/);
    expect(packageNoticeDetail('package.payment', { status: 'refund_due', amount: 100 })).toMatch(/вручную/);
    expect(packageNoticeDetail('booking.created', {})).toBe('');
  });
});

describe('подпись «По абонементу» в карточке записи', () => {
  it('списано, пока встреча в силе; отмена владельцем — вернулось; гостем — без обещаний', () => {
    expect(packageSessionNote({ status: 'active' }, { status: 'active' })).toBe('занятие списано с абонемента клиента');
    expect(packageSessionNote({ status: 'cancelled', cancelled_by: 'owner' }, { status: 'cancelled' })).toBe('вы отменили встречу — занятие вернулось на абонемент');
    expect(packageSessionNote({ status: 'cancelled', cancelled_by: 'guest' }, { status: 'cancelled' })).toBe('встреча отменена — остаток занятий в «Абонементах»');
    // Отменена одна встреча серии (запись в силе) — кто отменил, неизвестно.
    expect(packageSessionNote({ status: 'active' }, { status: 'cancelled' })).toBe('встреча отменена — остаток занятий в «Абонементах»');
  });
});
