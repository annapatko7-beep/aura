import { describe, expect, it, vi } from 'vitest';
import { makeIntegrationsApi } from './integrationsApi';
import type { SelfvoySocket } from './socket';

describe('makeIntegrationsApi', () => {
  it('шлёт telegram.*, payments.*, calendar.* с полями ровно как в протоколе', async () => {
    const request = vi.fn().mockResolvedValue({});
    const api = makeIntegrationsApi({ request } as unknown as SelfvoySocket);
    await api.telegramStatus();
    await api.telegramLink();
    await api.telegramUnlink();
    await api.paymentsStatus();
    await api.paymentsConnect('123456', 'test_secret');
    await api.paymentsDisconnect();
    await api.calendarFeed();
    await api.calendarFeedReset();
    await api.calendarFeedDisable();
    expect(request.mock.calls).toEqual([
      ['telegram.status', {}],
      ['telegram.link', {}],
      ['telegram.unlink', {}],
      ['payments.status', {}],
      ['payments.connect', { provider: 'yookassa', shop_id: '123456', secret_key: 'test_secret' }],
      ['payments.disconnect', {}],
      ['calendar.feed', {}],
      ['calendar.feedReset', {}],
      ['calendar.feedDisable', {}],
    ]);
  });

  it('возвращает ответ сервера как есть', async () => {
    const request = vi.fn().mockResolvedValue({ enabled: true, bot_username: 'selfvoy_bot', linked: true, username: 'anna' });
    const api = makeIntegrationsApi({ request } as unknown as SelfvoySocket);
    await expect(api.telegramStatus()).resolves.toEqual({ enabled: true, bot_username: 'selfvoy_bot', linked: true, username: 'anna' });
  });
});
