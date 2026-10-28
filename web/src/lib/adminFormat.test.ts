import { describe, expect, it } from 'vitest';
import type { AuditEntry } from '../net/adminApi';
import { adminDate, auditDetail, auditLabel, auditSubline, auditWho, count, isAuditAlert, pct } from './adminFormat';

const entry = (over: Partial<AuditEntry> = {}): AuditEntry => ({
  id: 1,
  user_id: 5,
  kind: 'login_ok',
  detail: {},
  remote_addr: '10.0.0.1',
  created_at: '2026-10-13T10:00:00Z',
  ...over,
});

// toLocaleString('ru-RU') разделяет разряды неразрывным пробелом — сравниваем без него.
const plain = (s: string) => s.replace(/[\s  ]/g, ' ');

describe('auditLabel / isAuditAlert', () => {
  it('известные события — по-русски, неизвестные — как есть', () => {
    expect(auditLabel('login_failed')).toBe('Неудачный вход');
    expect(auditLabel('admin_user_disabled')).toBe('Заблокирован');
    expect(auditLabel('something_new')).toBe('something_new');
  });

  it('неудачи и блокировки помечаются как тревожные', () => {
    expect(isAuditAlert('login_failed')).toBe(true);
    expect(isAuditAlert('refresh_reuse_detected')).toBe(true);
    expect(isAuditAlert('admin_user_disabled')).toBe(true);
    expect(isAuditAlert('login_ok')).toBe(false);
  });
});

describe('auditDetail', () => {
  it('пустой, null и не-объект дают пустую строку', () => {
    expect(auditDetail({})).toBe('');
    expect(auditDetail(null)).toBe('');
    expect(auditDetail(undefined)).toBe('');
  });

  it('роль подписывается по-русски, со старой ролью — стрелкой', () => {
    expect(auditDetail({ role: 'moderator' })).toBe('роль: Модератор');
    expect(auditDetail({ role: 'admin', old_role: 'user' })).toBe('роль: Пользователь → Администратор');
    expect(auditDetail({ role: 'owner' })).toBe('роль: owner');
  });

  it('собирает причину, число сессий, кем и кого; пустые и чужие типы пропускает', () => {
    expect(auditDetail({ reason: '  спам  ', revoked: 3, actor_email: 'a@x.io', target_email: 'b@x.io' })).toBe('причина: спам, сессий: 3, кем: a@x.io, кого: b@x.io');
    expect(auditDetail({ reason: '', revoked: '3', actor_email: 42 })).toBe('');
  });
});

describe('auditWho / auditSubline', () => {
  it('email, иначе id, иначе система', () => {
    expect(auditWho({ user_id: 5, user_email: 'u@x.io' })).toBe('u@x.io');
    expect(auditWho({ user_id: 5 })).toBe('id 5');
    expect(auditWho({ user_id: 0 })).toBe('система');
  });

  it('вторая строка: кто (по запросу) · адрес · детали, без пустых частей', () => {
    const e = entry({ user_email: 'u@x.io', detail: { reason: 'спам' } });
    expect(auditSubline(e)).toBe('10.0.0.1 · причина: спам');
    expect(auditSubline(e, true)).toBe('u@x.io · 10.0.0.1 · причина: спам');
    expect(auditSubline(entry({ remote_addr: '' }))).toBe('');
  });
});

describe('pct / count', () => {
  it('проценты округляются, деление на ноль — 0%', () => {
    expect(pct(1, 3)).toBe('33%');
    expect(pct(2, 3)).toBe('67%');
    expect(pct(5, 0)).toBe('0%');
    expect(pct(Number.NaN, 10)).toBe('0%');
  });

  it('числа — с разрядами, отсутствующие — прочерк', () => {
    expect(plain(count(1234567))).toBe('1 234 567');
    expect(count(0)).toBe('0');
    expect(count(undefined)).toBe('—');
    expect(count(null)).toBe('—');
  });
});

describe('adminDate', () => {
  const now = new Date(2026, 9, 13, 15, 0);

  it('в этом году — как when(), раньше — с годом', () => {
    expect(adminDate(new Date(2026, 9, 13, 9, 5).toISOString(), now)).toBe('сегодня, 09:05');
    expect(adminDate(new Date(2024, 2, 8, 12, 0).toISOString(), now)).toBe('8 марта 2024');
  });

  it('пустая или битая дата — пустая строка', () => {
    expect(adminDate('', now)).toBe('');
    expect(adminDate(null, now)).toBe('');
    expect(adminDate('not a date', now)).toBe('');
  });
});
