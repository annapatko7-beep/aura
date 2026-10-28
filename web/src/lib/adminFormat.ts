/*
  Подписи админ-панели: события журнала аудита, выжимка detail, проценты и даты.
  Чистые функции без React — проверяются юнит-тестами (adminFormat.test.ts).
*/
import { ROLE_LABEL, type AuditEntry, type Role } from '../net/adminApi';
import { parseDate, when } from './format';
import { dateLabel } from './time';

export const AUDIT_LABEL: Record<string, string> = {
  // вход и регистрация (server/src/authmanager.cpp)
  register: 'Регистрация',
  verification_sent: 'Отправлен код подтверждения',
  email_verified: 'Email подтверждён',
  login_ok: 'Вход',
  login_failed: 'Неудачный вход',
  login_blocked: 'Вход заблокирован: много попыток',
  login_2fa_required: 'Запрошен код 2FA',
  login_trusted_device: 'Вход с доверенного устройства',
  // пароль
  password_changed: 'Пароль изменён',
  password_change_failed: 'Неудачная смена пароля',
  password_reset_requested: 'Запрошен сброс пароля',
  password_reset: 'Пароль сброшен',
  password_reset_failed: 'Неудачный сброс пароля',
  password_rehashed: 'Хэш пароля обновлён',
  // сессии
  session_revoked: 'Сессия завершена',
  sessions_revoked_all: 'Все другие сессии завершены',
  refresh_rotated: 'Обновлён токен',
  refresh_reuse_detected: 'Повторное использование токена',
  // 2FA
  '2fa_setup_started': 'Начата настройка 2FA',
  '2fa_enabled': '2FA включена',
  '2fa_disabled': '2FA отключена',
  '2fa_confirm_failed': 'Неверный код при включении 2FA',
  '2fa_disable_failed': 'Неудачное отключение 2FA',
  '2fa_failed': 'Неверный код 2FA',
  '2fa_blocked': '2FA: много неверных кодов',
  '2fa_recovery_used': 'Вход по резервному коду',
  '2fa_recovery_failed': 'Неверный резервный код',
  '2fa_device_revoked': 'Доверенное устройство удалено',
  '2fa_devices_revoked_all': 'Все доверенные устройства удалены',
  // администрирование
  admin_role_changed: 'Изменена роль',
  admin_user_disabled: 'Заблокирован',
  admin_user_enabled: 'Разблокирован',
  admin_sessions_revoked: 'Сессии завершены администратором',
  role_bootstrap: 'Назначен администратором (SELFVOY_ADMIN_EMAILS)',
};

/** Человеческое название события; неизвестный kind показывается как есть. */
export const auditLabel = (kind: string): string => AUDIT_LABEL[kind] ?? kind;

/** События, на которые стоит обратить внимание: неудачи, подозрения, блокировки. */
const ALERT_KINDS = new Set([
  'login_failed',
  'login_blocked',
  'password_change_failed',
  'password_reset_failed',
  'refresh_reuse_detected',
  '2fa_failed',
  '2fa_blocked',
  '2fa_recovery_failed',
  '2fa_disable_failed',
  'admin_user_disabled',
]);

export const isAuditAlert = (kind: string): boolean => ALERT_KINDS.has(kind);

const roleName = (value: string) => ROLE_LABEL[value as Role] ?? value;

/** Короткая выжимка detail: роль, причина, число сессий, кто и кого. */
export function auditDetail(detail: Record<string, unknown> | null | undefined): string {
  if (!detail || typeof detail !== 'object') return '';
  const parts: string[] = [];
  if (typeof detail.role === 'string' && detail.role) {
    const from = typeof detail.old_role === 'string' && detail.old_role ? `${roleName(detail.old_role)} → ` : '';
    parts.push(`роль: ${from}${roleName(detail.role)}`);
  }
  if (typeof detail.reason === 'string' && detail.reason.trim()) parts.push(`причина: ${detail.reason.trim()}`);
  if (typeof detail.revoked === 'number') parts.push(`сессий: ${detail.revoked}`);
  if (typeof detail.actor_email === 'string' && detail.actor_email) parts.push(`кем: ${detail.actor_email}`);
  if (typeof detail.target_email === 'string' && detail.target_email) parts.push(`кого: ${detail.target_email}`);
  return parts.join(', ');
}

/** Чьё событие: email, иначе id, иначе «система» (user_id = 0). */
export function auditWho(entry: Pick<AuditEntry, 'user_id' | 'user_email'>): string {
  if (entry.user_email) return entry.user_email;
  return entry.user_id ? `id ${entry.user_id}` : 'система';
}

/** Вторая строка записи журнала: [кто ·] адрес · выжимка detail. */
export function auditSubline(entry: AuditEntry, showUser = false): string {
  return [showUser ? auditWho(entry) : '', entry.remote_addr, auditDetail(entry.detail)].filter(Boolean).join(' · ');
}

/** Доля в процентах, округлённая; при пустом total — «0%». */
export function pct(part: number, total: number): string {
  if (!total || !Number.isFinite(part) || !Number.isFinite(total)) return '0%';
  return `${Math.round((part / total) * 100)}%`;
}

/** Число с разрядами по-русски; нет значения — «—». */
export function count(value: number | null | undefined): string {
  return typeof value === 'number' && Number.isFinite(value) ? value.toLocaleString('ru-RU') : '—';
}

/** Дата для карточки пользователя: в этом году — как when(), раньше — «15 октября 2024». */
export function adminDate(iso: string | null | undefined, now = new Date()): string {
  const d = parseDate(iso);
  if (!d) return '';
  return d.getFullYear() === now.getFullYear() ? when(iso, now) : `${dateLabel(d)} ${d.getFullYear()}`;
}
