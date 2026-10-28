/*
  Методы админ-панели (docs/PROTOCOL.md → «Администрирование»). Права проверяет
  сервер: модератор видит пользователей и может блокировать обычных участников,
  администратор — ещё роли и журнал аудита.
*/
import type { User } from './api';
import type { SelfvoySocket } from './socket';

/** Роль пользователя — тот же тип, что у User.role в auth-ответах. */
export type Role = NonNullable<User['role']>;

export interface AdminUser {
  id: number;
  email: string;
  display_name: string;
  avatar_url: string;
  role: Role;
  is_active: boolean;
  email_verified: boolean;
  two_factor: boolean;
  created_at: string;
  last_seen_at: string;
}

export interface AdminStats {
  users_total: number;
  users_verified: number;
  users_disabled: number;
  users_2fa: number;
  users_new_7d: number;
  moderators: number;
  admins: number;
  sessions_active: number;
  online_now: number;
}

export interface AuditEntry {
  id: number;
  user_id: number;
  user_email?: string;
  kind: string;
  detail: Record<string, unknown>;
  remote_addr: string;
  created_at: string;
}

export interface AdminUserDetail {
  user: AdminUser;
  sessions_active: number;
  audit: AuditEntry[];
}

export type UserStatus = '' | 'active' | 'disabled' | 'unverified';

export function makeAdminApi(socket: SelfvoySocket) {
  const call = <T,>(type: string, payload: Record<string, unknown> = {}) => socket.request<T>(type, payload);
  return {
    stats: () => call<AdminStats>('admin.stats'),
    users: (query: string, role: Role | '', status: UserStatus, offset = 0, limit = 50) =>
      call<{ users: AdminUser[]; total: number }>('admin.users', { query, role, status, offset, limit }),
    user: (user_id: number) => call<AdminUserDetail>('admin.user', { user_id }),
    setActive: (user_id: number, active: boolean, reason: string) =>
      call<{ user: AdminUser }>('admin.setActive', { user_id, active, reason }),
    setRole: (user_id: number, role: Role) => call<{ user: AdminUser }>('admin.setRole', { user_id, role }),
    revokeSessions: (user_id: number) => call<{ revoked: number }>('admin.revokeSessions', { user_id }),
    audit: (user_id = 0, before_id = 0, limit = 100) => call<{ entries: AuditEntry[] }>('admin.audit', { user_id, before_id, limit }),
  };
}

export type AdminApi = ReturnType<typeof makeAdminApi>;

export const ROLE_LABEL: Record<Role, string> = { user: 'Пользователь', moderator: 'Модератор', admin: 'Администратор' };

/** Видит ли пользователь админ-панель: модератор или администратор. */
export function isStaff(role: Role | undefined | null): role is 'moderator' | 'admin' {
  return role === 'moderator' || role === 'admin';
}

/** Может ли actor менять статус/сессии target (то же правило, что на сервере). */
export function canModerate(actor: Role | undefined, actorId: number, target: AdminUser): boolean {
  if (!actor || actor === 'user' || target.id === actorId) return false;
  if (actor === 'admin') return true;
  return target.role === 'user';
}
