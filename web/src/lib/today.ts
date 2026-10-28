/*
  «Сегодня» владельца: следующая встреча и лента дня из настоящих данных —
  встречи записей (booking.list), задачи со сроком (tasks.list) и свободные окна
  страницы записи (booking.get по своему slug). Без React — тестируется в today.test.ts.
*/
import type { Task } from '../net/api';
import type { Booking, BookingPage, Occurrence } from '../net/bookingApi';
import type { InviteSlot } from '../net/inviteApi';
import { plural } from './format';
import { awaitingPayment, bookingMoney, paymentLabel, type StatusView } from './payment';
import { addDays, dateLabel, dayShort, hhmm, sameDay } from './time';

/** Цвет капсулы — по странице записи (тип встречи): 5 цветов по кругу. */
export const CAPSULE_COLORS = 5;

export interface DayBooking {
  kind: 'booking';
  key: string;
  start: Date;
  end: Date;
  booking: Booking;
  occurrence: Occurrence;
  color: number;
  status: StatusView;
}

export interface DayTask {
  kind: 'task';
  key: string;
  start: Date;
  task: Task;
}

export interface DayFree {
  kind: 'free';
  key: string;
  start: Date;
  end: Date;
  pageId: number;
}

export interface DayNow {
  kind: 'now';
  key: 'now';
  start: Date;
}

export type DayEntry = DayBooking | DayTask | DayFree | DayNow;

/** Слот свободного времени со страницы (вид гостя). */
export interface FreeSlot extends InviteSlot {
  pageId: number;
}

const parse = (iso: string | undefined) => {
  const t = iso ? Date.parse(iso) : Number.NaN;
  return Number.isNaN(t) ? null : new Date(t);
};

/** Номер цвета страницы (1..5): по порядку создания страниц. */
export function pageColor(pageId: number, pages: Pick<BookingPage, 'id'>[] | null | undefined): number {
  const ids = [...new Set((pages ?? []).map((p) => p.id))].sort((a, b) => a - b);
  const index = ids.indexOf(pageId);
  return ((index >= 0 ? index : Math.abs(pageId)) % CAPSULE_COLORS) + 1;
}

/**
  Статус встречи для ленты — всегда значок и слово. Важнее всего то, что требует
  действия: оплата; затем подтверждение гостя; новая запись (за сутки) — «внимание».
*/
export function occurrenceStatus(booking: Booking, occurrence: Occurrence, now = new Date()): StatusView {
  if (booking.status === 'cancelled' || occurrence.status === 'cancelled') return { label: 'Отменена', tone: 'red', icon: 'x' };
  if (awaitingPayment(booking.payment)) return paymentLabel(booking.payment?.status, 'owner') as StatusView;
  if (occurrence.confirmed_at) return { label: 'Подтвердил(а)', tone: 'green', icon: 'check' };
  const end = parse(occurrence.end) ?? parse(occurrence.start);
  if (end && end.getTime() <= now.getTime()) return { label: 'Прошла', tone: '', icon: 'check' };
  const created = parse(booking.created_at);
  if (created && now.getTime() - created.getTime() < 24 * 3_600_000) return { label: 'Новая запись', tone: 'new', icon: 'plus' };
  if (booking.payment?.status === 'paid') return { label: 'Оплачено', tone: 'green', icon: 'check' };
  return { label: 'Записан(а)', tone: '', icon: 'calendar' };
}

/** Ближайшая встреча: идущая сейчас или следующая (активная запись и встреча). */
export function nextUp(bookings: Booking[] | null | undefined, now = new Date()): { booking: Booking; occurrence: Occurrence; start: Date; end: Date } | null {
  let best: { booking: Booking; occurrence: Occurrence; start: Date; end: Date } | null = null;
  for (const booking of bookings ?? []) {
    if (booking.status !== 'active') continue;
    for (const occurrence of booking.occurrences ?? []) {
      if (occurrence.status !== 'active') continue;
      const start = parse(occurrence.start);
      const end = parse(occurrence.end) ?? start;
      if (!start || !end || end.getTime() <= now.getTime()) continue;
      if (!best || start.getTime() < best.start.getTime()) best = { booking, occurrence, start, end };
    }
  }
  return best;
}

/** «через 1 ч 13 мин», «идёт сейчас», «завтра в 10:00», «пт, 23 октября». */
export function untilText(start: Date, now = new Date(), end?: Date): string {
  const minutes = Math.round((start.getTime() - now.getTime()) / 60_000);
  if (minutes <= 0) return end && end.getTime() > now.getTime() ? 'идёт сейчас' : 'началась';
  if (sameDay(start, now)) {
    if (minutes < 60) return `через ${minutes} мин`;
    const h = Math.floor(minutes / 60);
    const m = minutes % 60;
    return `через ${h} ч${m ? ` ${m} мин` : ''}`;
  }
  if (sameDay(start, addDays(now, 1))) return `завтра в ${hhmm(start)}`;
  return `${dayShort(start).toLowerCase()}, ${dateLabel(start)}`;
}

/** Задачи, созданные записью (due_at = start встречи), в ленте не дублируем. */
function bookingMinutes(bookings: Booking[]): Set<number> {
  const set = new Set<number>();
  for (const b of bookings) for (const o of b.occurrences ?? []) {
    const t = Date.parse(o.start);
    if (!Number.isNaN(t)) set.add(Math.floor(t / 60_000));
  }
  return set;
}

/** Сколько свободных окон показывать в ленте дня. */
export const FREE_IN_DAY = 3;

/**
  Лента дня `day` (местный день устройства): встречи, задачи со сроком, свободные
  окна (не пересекаются со встречами, не больше FREE_IN_DAY, только будущие) и
  линия «сейчас», если день — сегодня. По времени.
*/
export function buildDay(
  input: { bookings?: Booking[] | null; tasks?: Task[] | null; free?: FreeSlot[] | null; pages?: Pick<BookingPage, 'id'>[] | null },
  day: Date,
  now = new Date(),
): DayEntry[] {
  const bookings = input.bookings ?? [];
  const entries: DayEntry[] = [];
  const busy: { start: number; end: number }[] = [];
  for (const booking of bookings) {
    for (const occurrence of booking.occurrences ?? []) {
      const start = parse(occurrence.start);
      if (!start || !sameDay(start, day)) continue;
      const end = parse(occurrence.end) ?? start;
      // Отменённые встречи — в списке записей; в ленте дня только живые.
      if (booking.status !== 'active' || occurrence.status !== 'active') continue;
      busy.push({ start: start.getTime(), end: end.getTime() });
      entries.push({
        kind: 'booking',
        key: `b${booking.id}-${occurrence.start}`,
        start,
        end,
        booking,
        occurrence,
        color: pageColor(booking.page_id, input.pages),
        status: occurrenceStatus(booking, occurrence, now),
      });
    }
  }
  const fromBookings = bookingMinutes(bookings);
  for (const task of input.tasks ?? []) {
    if (task.status !== 'pending') continue;
    const at = parse(task.due_at) ?? parse(task.remind_at);
    if (!at || !sameDay(at, day)) continue;
    if (fromBookings.has(Math.floor(at.getTime() / 60_000))) continue;
    entries.push({ kind: 'task', key: `t${task.id}`, start: at, task });
  }
  let free = 0;
  const seen = new Set<number>();
  for (const slot of [...(input.free ?? [])].sort((a, b) => Date.parse(a.start) - Date.parse(b.start))) {
    if (free >= FREE_IN_DAY) break;
    const start = parse(slot.start);
    const end = parse(slot.end);
    if (!start || !end || !sameDay(start, day) || start.getTime() <= now.getTime() || seen.has(start.getTime())) continue;
    if (busy.some((b) => start.getTime() < b.end && end.getTime() > b.start)) continue;
    seen.add(start.getTime());
    busy.push({ start: start.getTime(), end: end.getTime() });
    entries.push({ kind: 'free', key: `f${slot.pageId}-${slot.start}`, start, end, pageId: slot.pageId });
    free += 1;
  }
  entries.sort((a, b) => a.start.getTime() - b.start.getTime() || (a.kind === 'free' ? 1 : 0) - (b.kind === 'free' ? 1 : 0));
  if (sameDay(day, now) && entries.length > 0) {
    const index = entries.findIndex((e) => e.start.getTime() > now.getTime());
    const line: DayNow = { kind: 'now', key: 'now', start: now };
    if (index === -1) entries.push(line);
    else entries.splice(index, 0, line);
  }
  return entries;
}

/**
  «4 встречи · 6 000 ₽» — встречи дня и их стоимость: цена каждой встречи на момент
  записи (booking.price; старый сервер — цена страницы), одна валюта. Встречи по
  абонементу уже оплачены пакетом — в сумму не входят (но считаются встречами).
*/
export function daySummary(entries: DayEntry[], pages: Pick<BookingPage, 'id' | 'price' | 'currency'>[] | null | undefined): { text: string; amount: number; currency: string } {
  const meetings = entries.filter((e): e is DayBooking => e.kind === 'booking');
  let amount = 0;
  let currency = '';
  for (const m of meetings) {
    const money = bookingMoney(m.booking, (pages ?? []).find((p) => p.id === m.booking.page_id));
    if (money.kind !== 'money') continue;
    if (currency && money.currency !== currency) continue;
    currency = money.currency;
    amount += money.amount;
  }
  const count = meetings.length;
  const text = count ? `${count} ${plural(count, 'встреча', 'встречи', 'встреч')}` : 'Встреч нет';
  return { text, amount, currency };
}
