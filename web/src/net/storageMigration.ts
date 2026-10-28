/*
  Переименование Aura → Selfvoy: ключи хранилища сменили префикс «aura.» на
  «selfvoy.». Один раз при запуске (до того, как кто-то читает хранилище)
  значения переносятся на новые ключи — вошедшие пользователи остаются в
  системе, адрес сервера и ленты на устройстве сохраняются. Новое значение,
  если оно уже есть, не перезаписывается; старый ключ удаляется в любом случае.
*/

export const LEGACY_PREFIX = 'aura.';
export const PREFIX = 'selfvoy.';

type KeyStore = Pick<Storage, 'length' | 'key' | 'getItem' | 'setItem' | 'removeItem'>;

/** Переносит ключи «aura.*» в «selfvoy.*». Возвращает число перенесённых старых ключей. */
export function migrateStorageKeys(store: KeyStore | null): number {
  if (!store) return 0;
  try {
    const legacy: string[] = [];
    for (let i = 0; i < store.length; i += 1) {
      const key = store.key(i);
      if (key && key.startsWith(LEGACY_PREFIX)) legacy.push(key);
    }
    for (const key of legacy) {
      const next = PREFIX + key.slice(LEGACY_PREFIX.length);
      const value = store.getItem(key);
      if (value !== null && store.getItem(next) === null) store.setItem(next, value);
      store.removeItem(key);
    }
    return legacy.length;
  } catch {
    // Хранилище недоступно или переполнено — старые ключи остаются до следующего запуска.
    return 0;
  }
}

function storage(kind: 'localStorage' | 'sessionStorage'): Storage | null {
  try {
    return typeof window === 'undefined' ? null : window[kind];
  } catch {
    return null;
  }
}

/** Миграция localStorage и sessionStorage этого источника. */
export function migrateLegacyStorage(): void {
  migrateStorageKeys(storage('localStorage'));
  migrateStorageKeys(storage('sessionStorage'));
}
