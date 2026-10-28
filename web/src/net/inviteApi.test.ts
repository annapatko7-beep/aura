import { describe, expect, it, vi } from 'vitest';
import { makeInviteApi } from './inviteApi';
import type { SelfvoySocket } from './socket';

describe('makeInviteApi', () => {
  it('шлёт invite.* с полями ровно как в протоколе', async () => {
    const request = vi.fn().mockResolvedValue({});
    const api = makeInviteApi({ request } as unknown as SelfvoySocket);
    await api.create({ topic: 'Кофе', duration_minutes: 60, window_hours: 168 });
    await api.create({ topic: 'Кофе', duration_minutes: 30, window_hours: 48, note: 'У входа', guest_name: 'Аня' });
    await api.list();
    await api.list(10);
    await api.cancel(7);
    await api.get('tok');
    await api.respond('tok', { action: 'accept', start: '2026-10-15T07:00:00Z', guest_name: 'Аня' });
    await api.respond('tok', { action: 'decline', start: 'ignored', guest_name: 'Аня', guest_email: 'a@b.ru', comment: 'Занята' });
    expect(request.mock.calls).toEqual([
      ['invite.create', { topic: 'Кофе', duration_minutes: 60, window_hours: 168 }, 90_000],
      ['invite.create', { topic: 'Кофе', duration_minutes: 30, window_hours: 48, note: 'У входа', guest_name: 'Аня' }, 90_000],
      ['invite.list', { limit: 50 }],
      ['invite.list', { limit: 10 }],
      ['invite.cancel', { invite_id: 7 }],
      ['invite.get', { token: 'tok' }],
      ['invite.respond', { token: 'tok', action: 'accept', guest_name: 'Аня', start: '2026-10-15T07:00:00Z' }],
      ['invite.respond', { token: 'tok', action: 'decline', guest_name: 'Аня', guest_email: 'a@b.ru', comment: 'Занята' }],
    ]);
  });
});
