import { describe, expect, it, vi } from 'vitest';
import type { Tokens } from '../net/secureStore';
import { SelfvoyError } from '../net/socket';
import { reauthenticate, REFRESH_LOCK, withRefreshLock, type ReauthDeps } from './reauth';

const alex = { id: 1, email: 'alex@selfvoy.local', display_name: 'Алекс', avatar_url: '', bio: '' };

/** Поддельный сервер и хранилище токенов. handlers: тип → ответ (или ошибка). */
function setup(initial: Tokens | null, handlers: Record<string, (p: Record<string, unknown>) => unknown>, extra: Partial<ReauthDeps> = {}) {
  let stored = initial;
  const calls: string[] = [];
  const deps: ReauthDeps = {
    request: async <T,>(type: string, payload: Record<string, unknown> = {}) => {
      calls.push(type);
      const h = handlers[type];
      if (!h) throw new SelfvoyError('unauthorized', 'нет');
      return h(payload) as T;
    },
    load: async () => (stored ? { ...stored } : null),
    save: async (t) => {
      stored = t;
    },
    clear: async () => {
      stored = null;
    },
    lock: (fn) => fn(),
    device: 'Selfvoy в браузере',
    stale: () => false,
    ...extra,
  };
  return { deps, calls, tokens: () => stored, set: (t: Tokens | null) => (stored = t) };
}

const refuse = () => {
  throw new SelfvoyError('unauthorized', 'токен недействителен');
};

describe('reauthenticate', () => {
  it('нет токенов — экран входа, без запросов', async () => {
    const s = setup(null, {});
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'signedOut' });
    expect(s.calls).toEqual([]);
  });

  it('access-токен действует — вход без обновления', async () => {
    const s = setup({ access: 'A1', refresh: 'R1' }, { 'auth.token': () => ({ user: alex }) });
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'signedIn', user: alex });
    expect(s.calls).toEqual(['auth.token']);
  });

  it('старый сервер без user в ответе — профиль из auth.me', async () => {
    const s = setup({ access: 'A1', refresh: 'R1' }, { 'auth.token': () => ({}), 'auth.me': () => alex });
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'signedIn', user: alex });
  });

  it('access отклонён — refresh под блокировкой, новая пара сохраняется', async () => {
    const lock = vi.fn((fn: () => Promise<unknown>) => fn());
    const s = setup(
      { access: 'A1', refresh: 'R1' },
      {
        'auth.token': refuse,
        'auth.refresh': (p) => {
          expect(p.refresh_token).toBe('R1');
          return { token: 'A2', refresh_token: 'R2', user: alex };
        },
      },
      { lock: lock as ReauthDeps['lock'] },
    );
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'signedIn', user: alex });
    expect(lock).toHaveBeenCalledTimes(1);
    expect(s.tokens()).toEqual({ access: 'A2', refresh: 'R2' });
  });

  it('пока ждали блокировку, другая вкладка обновила токены — берём её пару, refresh не тратим', async () => {
    let s!: ReturnType<typeof setup>;
    s = setup(
      { access: 'A1', refresh: 'R1' },
      {
        'auth.token': (p) => (p.token === 'A2' ? { user: alex } : refuse()),
        'auth.refresh': () => {
          throw new Error('refresh не должен вызываться');
        },
      },
      {
        lock: async (fn) => {
          s.set({ access: 'A2', refresh: 'R2' }); // другая вкладка успела раньше
          return fn();
        },
      },
    );
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'signedIn', user: alex });
    expect(s.calls).toEqual(['auth.token', 'auth.token']);
    expect(s.tokens()).toEqual({ access: 'A2', refresh: 'R2' });
  });

  it('refresh отклонён — токены удаляются, экран входа', async () => {
    const s = setup({ access: 'A1', refresh: 'R1' }, { 'auth.token': refuse, 'auth.refresh': refuse });
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'signedOut' });
    expect(s.tokens()).toBeNull();
  });

  it('сетевая ошибка пробрасывается, токены не трогаются', async () => {
    const s = setup(
      { access: 'A1', refresh: 'R1' },
      {
        'auth.token': refuse,
        'auth.refresh': () => {
          throw new SelfvoyError('timeout', 'Сервер не ответил вовремя');
        },
      },
    );
    await expect(reauthenticate(s.deps)).rejects.toMatchObject({ code: 'timeout' });
    expect(s.tokens()).toEqual({ access: 'A1', refresh: 'R1' });
  });

  it('пользователь вышел или сменил сервер, пока шла авторизация — результат не применяется', async () => {
    let stale = false;
    const s = setup(
      { access: 'A1', refresh: 'R1' },
      {
        'auth.token': refuse,
        'auth.refresh': () => {
          stale = true;
          return { token: 'A2', refresh_token: 'R2', user: alex };
        },
      },
      { stale: () => stale },
    );
    expect(await reauthenticate(s.deps)).toEqual({ kind: 'stale' });
    expect(s.tokens()).toEqual({ access: 'A1', refresh: 'R1' });
  });
});

describe('withRefreshLock', () => {
  it('использует navigator.locks с общим именем, если он есть', async () => {
    const request = vi.fn((_name: string, fn: () => Promise<number>) => fn());
    expect(await withRefreshLock(async () => 42, { request: request as never })).toBe(42);
    expect(request).toHaveBeenCalledWith(REFRESH_LOCK, expect.any(Function));
  });
  it('без navigator.locks — просто выполняет', async () => {
    expect(await withRefreshLock(async () => 7, undefined)).toBe(7);
  });
});
