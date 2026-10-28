import { describe, expect, it, vi } from 'vitest';
import { makePackageApi } from './packageApi';
import type { SelfvoySocket } from './socket';

describe('makePackageApi', () => {
  it('шлёт package.* с полями ровно как в протоколе', async () => {
    const request = vi.fn().mockResolvedValue({});
    const api = makePackageApi({ request } as unknown as SelfvoySocket);
    await api.list();
    await api.list({ page_id: 2, status: 'active', limit: 200 });
    await api.issue({ page_id: 2, offer_id: 3, guest_name: 'Маша' });
    await api.issue({ page_id: 2, title: 'Месяц', sessions: 4, price: 800000, valid_days: 30, guest_name: 'Маша', guest_email: 'kid@b.ru', cc_email: 'mama@b.ru', paid: true });
    await api.issue({ page_id: 2, title: 'Подарок', sessions: 1, price: 0, guest_name: 'Маша', paid: false });
    await api.adjust(7, -1);
    await api.cancel(7);
    await api.paymentSet(7, 'waived');
    await api.get('tok');
    await api.paymentClaim('tok');
    expect(request.mock.calls).toEqual([
      ['package.list', {}],
      ['package.list', { page_id: 2, status: 'active', limit: 200 }],
      ['package.issue', { page_id: 2, guest_name: 'Маша', offer_id: 3 }],
      ['package.issue', { page_id: 2, guest_name: 'Маша', title: 'Месяц', sessions: 4, price: 800000, valid_days: 30, guest_email: 'kid@b.ru', cc_email: 'mama@b.ru', paid: true }],
      ['package.issue', { page_id: 2, guest_name: 'Маша', title: 'Подарок', sessions: 1 }],
      ['package.adjust', { package_id: 7, delta: -1 }],
      ['package.cancel', { package_id: 7 }],
      ['package.paymentSet', { package_id: 7, status: 'waived' }],
      ['package.get', { token: 'tok' }],
      ['package.paymentClaim', { token: 'tok' }],
    ]);
  });
});
