import { describe, expect, it, vi } from 'vitest';
import { makeBookingApi } from './bookingApi';
import type { SelfvoySocket } from './socket';

const PAGE = { title: 'Консультация', description: '', duration_minutes: 30, days_ahead: 14, max_per_day: 4, notice_hours: 2, max_series: 1, weekends: false };

describe('makeBookingApi', () => {
  it('шлёт booking.* и referral.get с полями ровно как в протоколе', async () => {
    const request = vi.fn().mockResolvedValue({});
    const api = makeBookingApi({ request } as unknown as SelfvoySocket);
    await api.createPage(PAGE);
    await api.listPages();
    await api.updatePage(3, { status: 'paused' });
    await api.deletePage(3);
    await api.list();
    await api.ownerCancel(9);
    await api.ownerCancel(9, '2026-10-20T07:00:00Z');
    await api.referral();
    await api.get('abcdefghij');
    await api.book('abcdefghij', { start: '2026-10-20T07:00:00Z', guest_name: 'Аня' });
    await api.book('abcdefghij', { start: '2026-10-20T07:00:00Z', guest_name: 'Аня', guest_email: 'a@b.ru', comment: 'Привет', count: 3 });
    await api.manage('tok');
    await api.reschedule('tok', '2026-10-21T07:00:00Z');
    await api.reschedule('tok', '2026-10-21T07:00:00Z', '2026-10-20T07:00:00Z');
    await api.cancel('tok');
    await api.cancel('tok', { occurrence: '2026-10-27T07:00:00Z', reason: 'Заболела' });
    await api.resendLink('abcdefghij', 'a@b.ru');
    await api.confirm('tok');
    await api.confirm('tok', '2026-10-20T07:00:00Z');
    await api.paymentClaim('tok');
    await api.paymentSet(9, 'paid');
    await api.paymentSet(9, 'refunded');
    await api.slotsPost(3);
    await api.slotsPost(3, { days: 14, format: 'vk' });
    await api.updatePage(3, { price: 250000, currency: 'RUB', prepay: 'deposit', deposit: 50000, guest_reminders: false, hide_branding: true });
    await api.book('abcdefghij', { start: '2026-10-20T07:00:00Z', guest_name: 'Аня', guest_email: 'a@b.ru', cc_email: 'mama@b.ru', package_id: 2, count: 2 });
    await api.book('abcdefghij', { start: '2026-10-20T07:00:00Z', package_token: 'ptok' });
    await api.book('abcdefghij', { start: '2026-10-20T07:00:00Z', package_token: 'ptok', package_id: 2, guest_name: 'Аня' });
    await api.updatePage(3, { packages: [{ id: 1, title: '8 занятий', sessions: 8, price: 1200000, valid_days: 90 }] });
    expect(request.mock.calls).toEqual([
      ['booking.pageCreate', PAGE],
      ['booking.pageList', {}],
      ['booking.pageUpdate', { page_id: 3, status: 'paused' }],
      ['booking.pageDelete', { page_id: 3 }],
      ['booking.list', { limit: 50 }],
      ['booking.ownerCancel', { booking_id: 9 }],
      ['booking.ownerCancel', { booking_id: 9, occurrence: '2026-10-20T07:00:00Z' }],
      ['referral.get', {}],
      ['booking.get', { slug: 'abcdefghij' }, 60_000],
      ['booking.book', { slug: 'abcdefghij', start: '2026-10-20T07:00:00Z', guest_name: 'Аня' }, 60_000],
      ['booking.book', { slug: 'abcdefghij', start: '2026-10-20T07:00:00Z', guest_name: 'Аня', guest_email: 'a@b.ru', comment: 'Привет', count: 3 }, 60_000],
      ['booking.manage', { token: 'tok' }, 60_000],
      ['booking.reschedule', { token: 'tok', start: '2026-10-21T07:00:00Z' }, 60_000],
      ['booking.reschedule', { token: 'tok', start: '2026-10-21T07:00:00Z', occurrence: '2026-10-20T07:00:00Z' }, 60_000],
      ['booking.cancel', { token: 'tok' }],
      ['booking.cancel', { token: 'tok', occurrence: '2026-10-27T07:00:00Z', reason: 'Заболела' }],
      ['booking.resendLink', { slug: 'abcdefghij', email: 'a@b.ru' }],
      ['booking.confirm', { token: 'tok' }],
      ['booking.confirm', { token: 'tok', occurrence: '2026-10-20T07:00:00Z' }],
      ['booking.paymentClaim', { token: 'tok' }],
      ['booking.paymentSet', { booking_id: 9, status: 'paid' }],
      ['booking.paymentSet', { booking_id: 9, status: 'refunded' }],
      ['booking.slotsPost', { page_id: 3 }, 60_000],
      ['booking.slotsPost', { page_id: 3, days: 14, format: 'vk' }, 60_000],
      ['booking.pageUpdate', { page_id: 3, price: 250000, currency: 'RUB', prepay: 'deposit', deposit: 50000, guest_reminders: false, hide_branding: true }],
      ['booking.book', { slug: 'abcdefghij', start: '2026-10-20T07:00:00Z', guest_name: 'Аня', guest_email: 'a@b.ru', cc_email: 'mama@b.ru', count: 2, package_id: 2 }, 60_000],
      // По абонементу имя можно не передавать; package_id и package_token вместе не уходят.
      ['booking.book', { slug: 'abcdefghij', start: '2026-10-20T07:00:00Z', package_token: 'ptok' }, 60_000],
      ['booking.book', { slug: 'abcdefghij', start: '2026-10-20T07:00:00Z', guest_name: 'Аня', package_token: 'ptok' }, 60_000],
      ['booking.pageUpdate', { page_id: 3, packages: [{ id: 1, title: '8 занятий', sessions: 8, price: 1200000, valid_days: 90 }] }],
    ]);
  });
});
