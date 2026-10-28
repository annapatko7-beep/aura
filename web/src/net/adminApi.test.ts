import { describe, expect, it, vi } from 'vitest';
import { canModerate, isStaff, makeAdminApi, type AdminUser, type Role } from './adminApi';
import type { SelfvoySocket } from './socket';

function target(id: number, role: Role): AdminUser {
  return {
    id,
    email: `u${id}@example.com`,
    display_name: `U${id}`,
    avatar_url: '',
    role,
    is_active: true,
    email_verified: true,
    two_factor: false,
    created_at: '2026-01-01T00:00:00Z',
    last_seen_at: '',
  };
}

describe('canModerate', () => {
  it('обычный пользователь и неизвестная роль не модерируют никого', () => {
    expect(canModerate(undefined, 1, target(2, 'user'))).toBe(false);
    expect(canModerate('user', 1, target(2, 'user'))).toBe(false);
  });

  it('себя не может модерировать никто, даже администратор', () => {
    expect(canModerate('admin', 1, target(1, 'admin'))).toBe(false);
    expect(canModerate('moderator', 5, target(5, 'moderator'))).toBe(false);
  });

  it('модератор управляет только обычными пользователями', () => {
    expect(canModerate('moderator', 1, target(2, 'user'))).toBe(true);
    expect(canModerate('moderator', 1, target(2, 'moderator'))).toBe(false);
    expect(canModerate('moderator', 1, target(2, 'admin'))).toBe(false);
  });

  it('администратор управляет всеми, кроме себя', () => {
    for (const role of ['user', 'moderator', 'admin'] as Role[]) expect(canModerate('admin', 1, target(2, role))).toBe(true);
  });
});

describe('isStaff', () => {
  it('панель видят только модераторы и администраторы', () => {
    expect(isStaff('admin')).toBe(true);
    expect(isStaff('moderator')).toBe(true);
    expect(isStaff('user')).toBe(false);
    expect(isStaff(undefined)).toBe(false);
    expect(isStaff(null)).toBe(false);
  });
});

describe('makeAdminApi', () => {
  it('шлёт admin.* с полями ровно как в протоколе', async () => {
    const request = vi.fn().mockResolvedValue({});
    const api = makeAdminApi({ request } as unknown as SelfvoySocket);
    await api.stats();
    await api.users('ann', 'moderator', 'disabled');
    await api.users('', '', '', 50, 25);
    await api.user(7);
    await api.setActive(7, false, 'спам');
    await api.setRole(7, 'moderator');
    await api.revokeSessions(7);
    await api.audit();
    await api.audit(7, 120, 20);
    expect(request.mock.calls).toEqual([
      ['admin.stats', {}],
      ['admin.users', { query: 'ann', role: 'moderator', status: 'disabled', offset: 0, limit: 50 }],
      ['admin.users', { query: '', role: '', status: '', offset: 50, limit: 25 }],
      ['admin.user', { user_id: 7 }],
      ['admin.setActive', { user_id: 7, active: false, reason: 'спам' }],
      ['admin.setRole', { user_id: 7, role: 'moderator' }],
      ['admin.revokeSessions', { user_id: 7 }],
      ['admin.audit', { user_id: 0, before_id: 0, limit: 100 }],
      ['admin.audit', { user_id: 7, before_id: 120, limit: 20 }],
    ]);
  });
});
