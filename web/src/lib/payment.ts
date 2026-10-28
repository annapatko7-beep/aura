/*
  Оплата записи (F4) и история встречи для владельца: подписи статусов (значок +
  слово, не только цвет), какие отметки доступны владельцу, шаги «истории» в
  карточке записи. Без React — тестируется в payment.test.ts.
*/
import type { Booking, BookingPage, CancelledBy, Occurrence, OwnerPaymentStatus, Payment, PaymentStatus, Policy, Prepay } from '../net/bookingApi';
import { when } from './format';
import { formatMoney } from './money';

export type Tone = 'green' | 'amber' | 'red' | 'accent' | 'new' | '';
/** Имя значка — компоненты сопоставляют его с иконкой. */
export type StatusIcon = 'check' | 'clock' | 'x' | 'undo' | 'minus' | 'hand' | 'plus' | 'bell' | 'calendar';

export interface StatusView {
  label: string;
  tone: Tone;
  icon: StatusIcon;
}

const PAYMENT: Record<PaymentStatus, { owner: string; guest: string; tone: Tone; icon: StatusIcon }> = {
  pending: { owner: 'Ждём оплату', guest: 'Ждём оплату', tone: 'amber', icon: 'clock' },
  claimed: { owner: 'Проверьте оплату', guest: 'Оплата отмечена', tone: 'amber', icon: 'hand' },
  paid: { owner: 'Оплачено', guest: 'Оплачено', tone: 'green', icon: 'check' },
  refunded: { owner: 'Возврат', guest: 'Деньги возвращены', tone: '', icon: 'undo' },
  waived: { owner: 'Без оплаты', guest: 'Оплата не нужна', tone: '', icon: 'minus' },
  expired: { owner: 'Не оплачено', guest: 'Оплата не поступила', tone: 'red', icon: 'x' },
};

/** Подпись статуса оплаты: владельцу — что сделать, гостю — что происходит. */
export function paymentLabel(status: PaymentStatus | string | undefined, who: 'owner' | 'guest' = 'owner'): StatusView | null {
  const view = status ? PAYMENT[status as PaymentStatus] : undefined;
  if (!view) return null;
  return { label: who === 'owner' ? view.owner : view.guest, tone: view.tone, icon: view.icon };
}

/** Владелец может отметить: ждём → «оплачено» / «без оплаты»; оплачено → «возврат». */
export function ownerPaymentActions(payment: Pick<Payment, 'status'> | null | undefined): OwnerPaymentStatus[] {
  switch (payment?.status) {
    case 'pending':
    case 'claimed':
      return ['paid', 'waived'];
    case 'paid':
      return ['refunded'];
    case 'waived':
    case 'expired':
      return ['paid'];
    default:
      return [];
  }
}

export const OWNER_PAYMENT_ACTION: Record<OwnerPaymentStatus, string> = {
  paid: 'Отметить оплату',
  waived: 'Без оплаты',
  refunded: 'Возврат',
};

/** Ждёт ли запись денег от гостя (для ленты дня и списка). */
export const awaitingPayment = (payment: Payment | null | undefined) => payment?.status === 'pending' || payment?.status === 'claimed';

// ------------------------------------------------------------ деньги встречи

/**
  Сколько стоит встреча записи для владельца. Источник — снимок цены booking.price
  (цена на момент записи; смена цены страницы её не меняет); старый сервер поля не
  присылает — тогда цена страницы. Запись по пакету (package_id) уже оплачена
  абонементом — денег за встречу нет. payment.amount — не цена, а сколько внесено
  онлайн (депозит или предоплата): его показывает paymentSummary.
*/
export type BookingMoney = { kind: 'package' } | { kind: 'money'; amount: number; currency: string } | { kind: 'free' };

export function bookingMoney(
  booking: Pick<Booking, 'price' | 'currency' | 'package_id'>,
  page?: Pick<BookingPage, 'price' | 'currency'> | null,
): BookingMoney {
  if (booking.package_id) return { kind: 'package' };
  const snapshot = typeof booking.price === 'number';
  const amount = snapshot ? (booking.price as number) : (page?.price ?? 0);
  const currency = (snapshot ? booking.currency : page?.currency) || page?.currency || 'RUB';
  return amount > 0 ? { kind: 'money', amount, currency } : { kind: 'free' };
}

export const BY_PACKAGE_LABEL = 'По абонементу';

/** «3 000 ₽» / «По абонементу» / "" (бесплатно или цена не задана). */
export function bookingMoneyLabel(
  booking: Pick<Booking, 'price' | 'currency' | 'package_id'>,
  page?: Pick<BookingPage, 'price' | 'currency'> | null,
): string {
  const money = bookingMoney(booking, page);
  if (money.kind === 'package') return BY_PACKAGE_LABEL;
  return money.kind === 'money' ? formatMoney(money.amount, money.currency) : '';
}

/**
  Оплата онлайн — отдельно от цены: «Депозит 1 000 ₽» + статус (значок и слово);
  line — одной строкой: «Депозит 1 000 ₽ · ждём оплату». null — оплаты нет.
*/
export function paymentSummary(payment: Payment | null | undefined): { text: string; status: StatusView | null; line: string } | null {
  if (!payment) return null;
  const what = payment.kind === 'deposit' ? 'Депозит' : payment.kind === 'package' ? 'Оплата пакета' : 'Предоплата';
  const text = `${what} ${formatMoney(payment.amount, payment.currency)}`;
  const status = paymentLabel(payment.status, 'owner');
  return { text, status, line: status ? `${text} · ${status.label.toLowerCase()}` : text };
}

/** «Предоплата 2 000 ₽» / «Депозит 500 ₽» / "" — для карточки услуги гостя. */
export function prepayLabel(page: { prepay?: Prepay; price?: number; deposit?: number; currency?: string }, count = 1): string {
  const currency = page.currency || 'RUB';
  if (page.prepay === 'full' && page.price) return `Предоплата ${formatMoney(page.price * Math.max(1, count), currency)}`;
  if (page.prepay === 'deposit' && page.deposit) return `Депозит ${formatMoney(page.deposit, currency)}`;
  return '';
}

/**
  Политика отмены для гостя — по строке на правило. cancel_hours 0 — отменять
  можно всегда; иначе — до срока с возвратом, позже — без возврата или нельзя.
*/
export function policyLines(policy: Policy | undefined, prepay: Prepay | undefined = 'none'): string[] {
  if (!policy) return [];
  const lines: string[] = [];
  const hours = Math.max(0, Math.round(policy.cancel_hours || 0));
  if (hours === 0) {
    lines.push('Перенести или отменить можно в любое время');
  } else {
    lines.push(`Перенести или отменить${prepay !== 'none' ? ' с возвратом оплаты' : ''} — не позже чем за ${hours} ч до встречи`);
    lines.push(policy.late_cancel === 'block' ? 'Позже — только по договорённости со специалистом' : prepay !== 'none' ? 'Позже — без возврата оплаты' : 'Позже отменить тоже можно, но лучше предупредить заранее');
  }
  const text = (policy.text || '').trim();
  if (text) lines.push(text);
  return lines;
}

// ------------------------------------------------------------------ история записи

export interface HistoryStep {
  key: string;
  title: string;
  detail: string;
  tone: 'done' | 'wait' | 'fail' | 'info';
}

const CANCELLED_BY: Record<Exclude<CancelledBy, ''>, string> = {
  guest: 'Гость отменил запись',
  owner: 'Вы отменили запись',
  system: 'Запись отменена: оплата не поступила',
};

/** Встреча, о которой рассказывает карточка: выбранная или ближайшая активная. */
function focusOccurrence(booking: Pick<Booking, 'occurrences'>, start: string | undefined, now: Date): Occurrence | undefined {
  const list = [...(booking.occurrences ?? [])].sort((a, b) => Date.parse(a.start) - Date.parse(b.start));
  if (start) return list.find((o) => o.start === start);
  return list.find((o) => o.status === 'active' && Date.parse(o.end || o.start) > now.getTime()) ?? list[list.length - 1];
}

/**
  Шаги истории записи для карточки владельца: запись → оплата → напоминания →
  «Приду» → отмена. `start` — встреча, на которую смотрим (серия: у каждой свои
  напоминания и подтверждение); без него — ближайшая.
*/
export function bookingHistory(booking: Booking, start?: string, now = new Date()): HistoryStep[] {
  const steps: HistoryStep[] = [{ key: 'created', title: 'Записался(ась) по ссылке', detail: when(booking.created_at, now), tone: 'done' }];
  const payment = booking.payment;
  if (payment) {
    const amount = formatMoney(payment.amount, payment.currency);
    const kind = payment.kind === 'deposit' ? 'депозит' : 'оплату';
    switch (payment.status) {
      case 'pending':
        steps.push({ key: 'payment', title: `Ждём ${kind} ${amount}`, detail: payment.due_at ? `до ${when(payment.due_at, now)}${payment.method === 'yookassa' ? ' · ЮKassa' : ' · по ссылке'}` : '', tone: 'wait' });
        break;
      case 'claimed':
        steps.push({ key: 'payment', title: `Гость отметил: оплатил(а) ${amount}`, detail: 'Проверьте поступление и отметьте оплату', tone: 'wait' });
        break;
      case 'paid':
        steps.push({ key: 'payment', title: `Оплачено ${amount}`, detail: payment.paid_at ? when(payment.paid_at, now) : '', tone: 'done' });
        break;
      case 'waived':
        steps.push({ key: 'payment', title: 'Без оплаты', detail: 'Вы отметили, что оплата не нужна', tone: 'info' });
        break;
      case 'refunded':
        steps.push({ key: 'payment', title: `Возврат ${amount}`, detail: payment.refunded_at ? when(payment.refunded_at, now) : '', tone: 'info' });
        break;
      case 'expired':
        steps.push({ key: 'payment', title: 'Оплата не поступила к сроку', detail: payment.due_at ? `срок был ${when(payment.due_at, now)}` : '', tone: 'fail' });
        break;
    }
  }
  const focus = focusOccurrence(booking, start, now);
  if (focus) {
    const reminders = (booking.reminders ?? []).filter((r) => r.start === focus.start).sort((a, b) => Date.parse(a.due_at) - Date.parse(b.due_at));
    for (const r of reminders) {
      const what = r.kind === '24h' ? 'за сутки' : 'за 2 часа';
      if (r.sent_at) steps.push({ key: `rem-${r.kind}`, title: `Напоминание ${what} отправлено`, detail: when(r.sent_at, now), tone: 'done' });
      else if (booking.status === 'active' && focus.status === 'active') steps.push({ key: `rem-${r.kind}`, title: `Напоминание ${what}`, detail: `запланировано на ${when(r.due_at, now)}`, tone: 'info' });
    }
    if (focus.confirmed_at) steps.push({ key: 'confirmed', title: 'Подтвердил(а): «Приду»', detail: when(focus.confirmed_at, now), tone: 'done' });
    else if (booking.status === 'active' && focus.status === 'active' && Date.parse(focus.start) > now.getTime()) steps.push({ key: 'confirmed', title: 'Ещё не подтвердил(а)', detail: 'Кнопка «Приду» — в напоминании', tone: 'info' });
    if (focus.status === 'cancelled' && booking.status === 'active') steps.push({ key: 'cancelled', title: 'Эта встреча отменена', detail: '', tone: 'fail' });
  }
  if (booking.status === 'cancelled') {
    steps.push({ key: 'cancelled', title: CANCELLED_BY[(booking.cancelled_by || 'guest') as Exclude<CancelledBy, ''>] ?? 'Запись отменена', detail: when(booking.updated_at, now), tone: 'fail' });
  }
  return steps;
}
