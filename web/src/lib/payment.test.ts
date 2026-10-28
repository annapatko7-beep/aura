import { describe, expect, it } from 'vitest';
import type { Booking, Payment } from '../net/bookingApi';
import { formatMoney } from './money';
import { awaitingPayment, bookingHistory, bookingMoney, bookingMoneyLabel, ownerPaymentActions, paymentLabel, paymentSummary, policyLines, prepayLabel } from './payment';

const NOW = new Date('2026-10-20T09:00:00Z');
const payment = (status: Payment['status'], extra: Partial<Payment> = {}): Payment => ({
  status,
  amount: 200000,
  currency: 'RUB',
  kind: 'full',
  method: 'link',
  action_url: 'https://pay.example/x',
  due_at: '2026-10-20T10:00:00Z',
  paid_at: '',
  refunded_at: '',
  ...extra,
});
const booking = (extra: Partial<Booking> = {}): Booking => ({
  id: 7,
  page_id: 1,
  page_title: 'Урок',
  guest_name: 'Мария',
  guest_email: '',
  guest_comment: '',
  status: 'active',
  cancelled_by: '',
  reschedules: 0,
  occurrences: [{ start: '2026-10-21T12:00:00Z', end: '2026-10-21T13:00:00Z', status: 'active', confirmed_at: '' }],
  created_at: '2026-10-19T15:40:00Z',
  updated_at: '2026-10-19T15:40:00Z',
  ...extra,
});

describe('подписи статусов оплаты', () => {
  it('у каждого статуса — слово и значок', () => {
    expect(paymentLabel('pending')).toEqual({ label: 'Ждём оплату', tone: 'amber', icon: 'clock' });
    expect(paymentLabel('claimed')?.label).toBe('Проверьте оплату');
    expect(paymentLabel('claimed', 'guest')?.label).toBe('Оплата отмечена');
    expect(paymentLabel('paid')).toEqual({ label: 'Оплачено', tone: 'green', icon: 'check' });
    expect(paymentLabel('refunded')?.icon).toBe('undo');
    expect(paymentLabel('waived')?.label).toBe('Без оплаты');
    expect(paymentLabel('expired', 'guest')).toEqual({ label: 'Оплата не поступила', tone: 'red', icon: 'x' });
    expect(paymentLabel('unknown')).toBeNull();
    expect(paymentLabel(undefined)).toBeNull();
  });

  it('действия владельца зависят от статуса', () => {
    expect(ownerPaymentActions(payment('pending'))).toEqual(['paid', 'waived']);
    expect(ownerPaymentActions(payment('claimed'))).toEqual(['paid', 'waived']);
    expect(ownerPaymentActions(payment('paid'))).toEqual(['refunded']);
    expect(ownerPaymentActions(payment('waived'))).toEqual(['paid']);
    expect(ownerPaymentActions(payment('refunded'))).toEqual([]);
    expect(ownerPaymentActions(null)).toEqual([]);
  });

  it('ждём денег — pending и claimed', () => {
    expect(awaitingPayment(payment('pending'))).toBe(true);
    expect(awaitingPayment(payment('claimed'))).toBe(true);
    expect(awaitingPayment(payment('paid'))).toBe(false);
    expect(awaitingPayment(null)).toBe(false);
  });
});

describe('деньги встречи: цена на момент записи и оплата отдельно', () => {
  const page = { price: 500000, currency: 'RUB' as const };
  it('цена — снимок booking.price, а не текущая цена страницы и не payment.amount', () => {
    const b = booking({ price: 300000, currency: 'RUB', payment: payment('pending', { kind: 'deposit', amount: 100000 }) });
    expect(bookingMoney(b, page)).toEqual({ kind: 'money', amount: 300000, currency: 'RUB' });
    expect(bookingMoneyLabel(b, page)).toBe(formatMoney(300000, 'RUB'));
  });
  it('по абонементу — «По абонементу» вместо денег', () => {
    const b = booking({ price: 0, package_id: 4 });
    expect(bookingMoney(b, page)).toEqual({ kind: 'package' });
    expect(bookingMoneyLabel(b, page)).toBe('По абонементу');
    expect(bookingMoneyLabel(booking({ package_id: 4 }), page)).toBe('По абонементу'); // старый сервер
  });
  it('price 0 — бесплатно; нет поля (старый сервер) — цена страницы', () => {
    expect(bookingMoney(booking({ price: 0, currency: 'RUB' }), page)).toEqual({ kind: 'free' });
    expect(bookingMoneyLabel(booking({ price: 0 }), page)).toBe('');
    expect(bookingMoney(booking(), page)).toEqual({ kind: 'money', amount: 500000, currency: 'RUB' });
    expect(bookingMoney(booking(), null)).toEqual({ kind: 'free' });
    expect(bookingMoney(booking({ price: 2500, currency: 'EUR' }), page)).toEqual({ kind: 'money', amount: 2500, currency: 'EUR' });
  });
  it('оплата онлайн: вид и сумма + статус словом', () => {
    const deposit = paymentSummary(payment('pending', { kind: 'deposit', amount: 100000 }));
    expect(deposit?.text).toBe(`Депозит ${formatMoney(100000, 'RUB')}`);
    expect(deposit?.status).toEqual({ label: 'Ждём оплату', tone: 'amber', icon: 'clock' });
    expect(deposit?.line).toBe(`Депозит ${formatMoney(100000, 'RUB')} · ждём оплату`);
    expect(paymentSummary(payment('paid', { amount: 300000 }))?.line).toBe(`Предоплата ${formatMoney(300000, 'RUB')} · оплачено`);
    expect(paymentSummary(null)).toBeNull();
  });
});

describe('предоплата и политика для гостя', () => {
  it('предоплата за серию и депозит', () => {
    expect(prepayLabel({ prepay: 'full', price: 200000, currency: 'RUB' }).replace(/ /g, ' ')).toBe('Предоплата 2 000 ₽');
    expect(prepayLabel({ prepay: 'full', price: 200000 }, 3).replace(/ /g, ' ')).toBe('Предоплата 6 000 ₽');
    expect(prepayLabel({ prepay: 'deposit', price: 200000, deposit: 50000 }, 3).replace(/ /g, ' ')).toBe('Депозит 500 ₽');
    expect(prepayLabel({ prepay: 'none', price: 200000 })).toBe('');
  });

  it('политика отмены по строкам', () => {
    expect(policyLines(undefined)).toEqual([]);
    expect(policyLines({ cancel_hours: 0, late_cancel: 'allow', text: '' })).toEqual(['Перенести или отменить можно в любое время']);
    expect(policyLines({ cancel_hours: 12, late_cancel: 'block', text: ' Опоздание — до 10 минут ' }, 'full')).toEqual([
      'Перенести или отменить с возвратом оплаты — не позже чем за 12 ч до встречи',
      'Позже — только по договорённости со специалистом',
      'Опоздание — до 10 минут',
    ]);
    expect(policyLines({ cancel_hours: 24, late_cancel: 'allow', text: '' }, 'deposit')[1]).toBe('Позже — без возврата оплаты');
  });
});

describe('история записи', () => {
  it('запись, ожидание оплаты, напоминания, подтверждение', () => {
    const steps = bookingHistory(
      booking({
        payment: payment('pending'),
        reminders: [
          { start: '2026-10-21T12:00:00Z', kind: '2h', due_at: '2026-10-21T10:00:00Z', sent_at: '' },
          { start: '2026-10-21T12:00:00Z', kind: '24h', due_at: '2026-10-20T12:00:00Z', sent_at: '2026-10-20T08:30:00Z' },
        ],
      }),
      undefined,
      NOW,
    );
    expect(steps.map((s) => [s.key, s.tone])).toEqual([
      ['created', 'done'],
      ['payment', 'wait'],
      ['rem-24h', 'done'],
      ['rem-2h', 'info'],
      ['confirmed', 'info'],
    ]);
    expect(steps[1].title.replace(/ /g, ' ')).toBe('Ждём оплату 2 000 ₽');
  });

  it('подтверждение, оплата и отмена системой', () => {
    const confirmed = bookingHistory(
      booking({ payment: payment('paid', { paid_at: '2026-10-19T16:00:00Z', kind: 'deposit' }), occurrences: [{ start: '2026-10-21T12:00:00Z', end: '2026-10-21T13:00:00Z', status: 'active', confirmed_at: '2026-10-20T08:40:00Z' }] }),
      undefined,
      NOW,
    );
    expect(confirmed.find((s) => s.key === 'confirmed')?.title).toBe('Подтвердил(а): «Приду»');
    expect(confirmed.find((s) => s.key === 'payment')?.tone).toBe('done');

    const expired = bookingHistory(booking({ status: 'cancelled', cancelled_by: 'system', payment: payment('expired') }), undefined, NOW);
    expect(expired.at(-1)).toMatchObject({ key: 'cancelled', title: 'Запись отменена: оплата не поступила', tone: 'fail' });
    expect(expired.find((s) => s.key === 'payment')?.tone).toBe('fail');
  });
});
