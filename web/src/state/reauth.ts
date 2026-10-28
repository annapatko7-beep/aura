/*
  Повторная авторизация на свежем соединении: auth.token, при отказе —
  auth.refresh (ротация refresh-токена).

  Refresh-токен одноразовый: если две вкладки (или окно и вкладка) обновят его
  одновременно, сервер увидит повторное использование и отзовёт всю цепочку —
  пользователь вылетит отовсюду. Поэтому обновление идёт под общей блокировкой
  (navigator.locks, общая для всех вкладок источника), а внутри неё токены
  перечитываются: если другая вкладка уже обновила их, пока мы ждали, —
  берём её пару, а не тратим старый refresh-токен ещё раз.

  Сетевые ошибки пробрасываются (соединение переподключится и попробует снова),
  токены при этом не трогаются.
*/
import type { AuthPayload, User } from '../net/api';
import type { Tokens } from '../net/secureStore';
import { isNetworkError } from '../net/socket';

export const REFRESH_LOCK = 'selfvoy.auth.refresh';

type LockManagerLike = { request<T>(name: string, callback: () => Promise<T>): Promise<T> };

/** Выполнить fn под межвкладочной блокировкой обновления токенов (если браузер её умеет). */
export function withRefreshLock<T>(fn: () => Promise<T>, locks: LockManagerLike | undefined = navigatorLocks()): Promise<T> {
  if (locks && typeof locks.request === 'function') return locks.request(REFRESH_LOCK, fn);
  return fn();
}

function navigatorLocks(): LockManagerLike | undefined {
  if (typeof navigator === 'undefined') return undefined;
  const locks = (navigator as { locks?: LockManagerLike }).locks;
  return locks && typeof locks.request === 'function' ? locks : undefined;
}

export interface ReauthDeps {
  request<T>(type: string, payload?: Record<string, unknown>): Promise<T>;
  load(): Promise<Tokens | null>;
  save(tokens: Tokens): Promise<void>;
  clear(): Promise<void>;
  lock<T>(fn: () => Promise<T>): Promise<T>;
  /** Имя устройства для auth.refresh. */
  device: string;
  /** Пока шла авторизация, пользователь вышел или сменил сервер — результат не применять. */
  stale(): boolean;
}

export type ReauthResult = { kind: 'signedIn'; user: User } | { kind: 'signedOut' } | { kind: 'stale' };

const same = (a: Tokens | null, b: Tokens | null) => Boolean(a && b && a.access === b.access && a.refresh === b.refresh);

export async function reauthenticate(deps: ReauthDeps): Promise<ReauthResult> {
  const tokens = await deps.load();
  if (deps.stale()) return { kind: 'stale' };
  if (!tokens) return { kind: 'signedOut' };

  /** auth.token: профиль при успехе, null при отказе; сетевые ошибки — наружу. */
  const byAccess = async (access: string): Promise<User | null> => {
    if (!access) return null;
    try {
      const reply = await deps.request<{ user?: User }>('auth.token', { token: access });
      // Старые серверы на свежем соединении отдают пустой ответ — профиль берём из auth.me.
      return reply.user ?? (await deps.request<User>('auth.me'));
    } catch (error) {
      if (isNetworkError(error)) throw error;
      return null;
    }
  };

  const user = await byAccess(tokens.access);
  if (deps.stale()) return { kind: 'stale' };
  if (user) return { kind: 'signedIn', user };

  return deps.lock(async (): Promise<ReauthResult> => {
    const now = await deps.load();
    if (deps.stale()) return { kind: 'stale' };
    if (!now) return { kind: 'signedOut' };
    // Другая вкладка обновила токены, пока мы ждали блокировку, — пробуем её пару.
    if (!same(now, tokens)) {
      const fresh = await byAccess(now.access);
      if (deps.stale()) return { kind: 'stale' };
      if (fresh) return { kind: 'signedIn', user: fresh };
    }
    if (now.refresh) {
      try {
        const renewed = await deps.request<AuthPayload>('auth.refresh', { refresh_token: now.refresh, device: deps.device });
        if (deps.stale()) return { kind: 'stale' };
        await deps.save({ access: renewed.token, refresh: renewed.refresh_token });
        return { kind: 'signedIn', user: renewed.user };
      } catch (error) {
        if (isNetworkError(error)) throw error;
      }
    }
    // Токены отозваны или истекли. Удаляем их, только если это всё ещё они
    // (другая вкладка могла за это время войти заново).
    if (deps.stale()) return { kind: 'stale' };
    if (same(await deps.load(), now)) await deps.clear();
    return { kind: 'signedOut' };
  });
}
