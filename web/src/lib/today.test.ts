import { describe, expect, it } from 'vitest';
import type { Task } from '../net/api';
import type { Booking, Payment } from '../net/bookingApi';
import { buildDay, daySummary, nextUp, occurrenceStatus, pageColor, untilText } from './today';

// Местное время устройства — тесты не зависят от пояса машины.
const at = (h: number, m = 0, day = 20) => new Date(2026, 9, day, h, m);
const iso = (h: number, m = 0, day = 20) => at(h, m, day).toISOString();
const NOW = at(13, 47);

const pay = (status: Payment['status']): Payment => ({ status, amount: 200000, currency: 'RUB', kind: 'full', method: 'link', action_url: '', due_at: '', paid_at: '', refunded_at: '' });

let nextId = 1;
const booking = (h: number, extra: Partial<Booking> = {}, day = 20): Booking => ({
  id: nextId++,
  page_id: 1,
  page_title: 'Урок',
  guest_name: 'Гость',
  guest_email: '',
  guest_comment: '',
  status: 'active',
  cancelled_by: '',
  reschedules: 0,
  occurrences: [{ start: iso(h, 0, day), end: iso(h + 1, 0, day), status: 'active', confirmed_at: '' }],
  created_at: at(9, 0, 10).toISOString(),
  updated_at: at(9, 0, 10).toISOString(),
  payment: null,
  ...extra,
});
const task = (id: number, h: number, m = 0, extra: Partial<Task> = {}): Task => ({ id, title: `Задача ${id}`, notes: '', status: 'pending', due_at: iso(h, m), remind_at: '', ...extra });

describe('лента дня', () => {
  it('встречи, задачи, свободные окна и линия «сейчас» — по времени', () => {
    const b10 = booking(10, { occurrences: [{ start: iso(10), end: iso(11), status: 'active', confirmed_at: iso(9) }] });
    const b15 = booking(15, { page_id: 2 });
    const entries = buildDay(
      {
        bookings: [b10, b15, booking(12, { status: 'cancelled', cancelled_by: 'guest' }), booking(10, {}, 21)],
        // Задача 15:00 создана записью — не дублируем; задача без срока и выполненная — не в ленте.
        tasks: [task(1, 14, 30), task(2, 15), task(3, 9, 0, { due_at: '' }), task(4, 16, 0, { status: 'done' })],
        free: [
          { pageId: 1, start: iso(11, 30), end: iso(12, 30) },
          { pageId: 1, start: iso(15, 30), end: iso(16, 30) }, // пересекается со встречей 15:00
          { pageId: 1, start: iso(17), end: iso(18) },
          { pageId: 1, start: iso(18), end: iso(19) },
          { pageId: 1, start: iso(19), end: iso(20) },
          { pageId: 1, start: iso(20), end: iso(21) },
        ],
        pages: [{ id: 1 }, { id: 2 }],
      },
      at(0),
      NOW,
    );
    expect(entries.map((e) => `${e.kind}@${e.start.getHours()}:${String(e.start.getMinutes()).padStart(2, '0')}`)).toEqual([
      'booking@10:00',
      'now@13:47',
      'task@14:30',
      'booking@15:00',
      'free@17:00',
      'free@18:00',
      'free@19:00',
    ]);
    // Окно 11:30 уже в прошлом — не предлагаем.
    const first = entries[0];
    expect(first.kind === 'booking' && first.status).toEqual({ label: 'Подтвердил(а)', tone: 'green', icon: 'check' });
    const second = entries.find((e) => e.kind === 'booking' && e.booking.id === b15.id);
    expect(second?.kind === 'booking' && second.color).toBe(2);
  });

  it('пустой день — без линии «сейчас»', () => {
    expect(buildDay({ bookings: [], tasks: [] }, at(0), NOW)).toEqual([]);
  });
});

describe('статус встречи', () => {
  const occ = { start: iso(15), end: iso(16), status: 'active' as const, confirmed_at: '' };
  it('оплата важнее подтверждения, новая запись — «внимание»', () => {
    expect(occurrenceStatus(booking(15, { payment: pay('pending') }), occ, NOW).label).toBe('Ждём оплату');
    expect(occurrenceStatus(booking(15, { payment: pay('claimed') }), { ...occ, confirmed_at: iso(9) }, NOW).label).toBe('Проверьте оплату');
    expect(occurrenceStatus(booking(15), { ...occ, confirmed_at: iso(9) }, NOW).tone).toBe('green');
    expect(occurrenceStatus(booking(15, { created_at: at(12).toISOString() }), occ, NOW)).toEqual({ label: 'Новая запись', tone: 'new', icon: 'plus' });
    expect(occurrenceStatus(booking(15, { payment: pay('paid') }), occ, NOW).label).toBe('Оплачено');
    expect(occurrenceStatus(booking(15), occ, NOW).label).toBe('Записан(а)');
    expect(occurrenceStatus(booking(15), { ...occ, status: 'cancelled' }, NOW).label).toBe('Отменена');
  });
});

describe('следующая встреча', () => {
  it('идущая сейчас или ближайшая активная', () => {
    const now = at(10, 30);
    const running = booking(10);
    const later = booking(15);
    expect(nextUp([later, running], now)?.booking.id).toBe(running.id);
    expect(nextUp([later, booking(9)], now)?.booking.id).toBe(later.id);
    expect(nextUp([booking(15, { status: 'cancelled' })], now)).toBeNull();
    expect(nextUp(null, now)).toBeNull();
  });

  it('сколько осталось', () => {
    expect(untilText(at(15), NOW)).toBe('через 1 ч 13 мин');
    expect(untilText(at(14), NOW)).toBe('через 13 мин');
    expect(untilText(at(15, 47), NOW)).toBe('через 2 ч');
    expect(untilText(at(13), NOW, at(14))).toBe('идёт сейчас');
    expect(untilText(at(10, 0, 21), NOW)).toBe('завтра в 10:00');
    expect(untilText(at(10, 0, 23), NOW)).toBe('пт, 23 октября');
  });
});

describe('итог дня и цвета', () => {
  it('число встреч и сумма по ценам страниц', () => {
    const entries = buildDay({ bookings: [booking(10), booking(15, { page_id: 2 }), booking(17, { page_id: 3 })] }, at(0), NOW);
    const sum = daySummary(entries, [
      { id: 1, price: 200000, currency: 'RUB' },
      { id: 2, price: 150000, currency: 'RUB' },
      { id: 3, price: 0 },
    ]);
    expect(sum).toEqual({ text: '3 встречи', amount: 350000, currency: 'RUB' });
    expect(daySummary([], []).text).toBe('Встреч нет');
  });

  it('сумма — по цене на момент записи; встречи по абонементу не входят', () => {
    const pages = [{ id: 1, price: 500000, currency: 'RUB' as const }];
    const entries = buildDay(
      {
        bookings: [
          booking(9, { price: 200000, currency: 'RUB' }), // цена до подорожания страницы
          booking(10, { price: 200000, currency: 'RUB', payment: pay('pending') }), // депозит/предоплата — не цена
          booking(12, { price: 0, currency: 'RUB', package_id: 7 }), // по абонементу
          booking(15, { price: 0, currency: 'RUB' }), // бесплатно — цену страницы не подставляем
          booking(17, { price: 200000, currency: 'RUB', status: 'cancelled', cancelled_by: 'guest' }),
        ],
      },
      at(0),
      NOW,
    );
    expect(daySummary(entries, pages)).toEqual({ text: '4 встречи', amount: 400000, currency: 'RUB' });
  });

  it('старый сервер без price — цена страницы; пакет без price — тоже не в сумме', () => {
    const entries = buildDay({ bookings: [booking(10), booking(12, { package_id: 3 })] }, at(0), NOW);
    expect(daySummary(entries, [{ id: 1, price: 300000, currency: 'EUR' }])).toEqual({ text: '2 встречи', amount: 300000, currency: 'EUR' });
  });

  it('другая валюта в сумму не смешивается', () => {
    const entries = buildDay({ bookings: [booking(10, { price: 100000, currency: 'RUB' }), booking(12, { price: 5000, currency: 'EUR' })] }, at(0), NOW);
    expect(daySummary(entries, [])).toEqual({ text: '2 встречи', amount: 100000, currency: 'RUB' });
  });

  it('цвет страницы по порядку создания', () => {
    const pages = [{ id: 9 }, { id: 3 }, { id: 5 }];
    expect(pageColor(3, pages)).toBe(1);
    expect(pageColor(5, pages)).toBe(2);
    expect(pageColor(9, pages)).toBe(3);
    expect(pageColor(42, null)).toBeGreaterThanOrEqual(1);
  });
});
