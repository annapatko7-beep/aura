/*
  Кэш данных пользователя на устройстве: последний профиль (чтобы приложение
  открывалось офлайн) и локальные ленты — диалог с Selfvoy и история переговоров.

  Всё это удаляется при выходе и при смене адреса сервера: id пользователя
  уникален только в пределах одного сервера, и лента пользователя 5 с сервера A
  не должна достаться пользователю 5 с сервера B.
*/
import type { User } from './api';

export const USER_KEY = 'selfvoy.user';
const PER_USER_PREFIXES = ['selfvoy.dialog.', 'selfvoy.negotiations.'];

type KeyStore = Pick<Storage, 'length' | 'key' | 'removeItem'>;

function storage(): Storage | null {
  try {
    return typeof localStorage === 'undefined' ? null : localStorage;
  } catch {
    return null;
  }
}

export function dialogKey(userId: number): string {
  return `${PER_USER_PREFIXES[0]}${userId}`;
}

export function negotiationsKey(userId: number): string {
  return `${PER_USER_PREFIXES[1]}${userId}`;
}

export function cachedUser(): User | null {
  try {
    const raw = storage()?.getItem(USER_KEY);
    return raw ? (JSON.parse(raw) as User) : null;
  } catch {
    return null;
  }
}

export function cacheUser(user: User | null): void {
  try {
    const s = storage();
    if (!s) return;
    if (user) s.setItem(USER_KEY, JSON.stringify(user));
    else s.removeItem(USER_KEY);
  } catch {
    /* без кэша профиль просто подгрузится после подключения */
  }
}

/** Удаляет профиль и ленты всех пользователей этого устройства. */
export function clearUserCaches(store: KeyStore | null = storage()): void {
  if (!store) return;
  try {
    const doomed: string[] = [];
    for (let i = 0; i < store.length; i += 1) {
      const key = store.key(i);
      if (key && (key === USER_KEY || PER_USER_PREFIXES.some((p) => key.startsWith(p)))) doomed.push(key);
    }
    doomed.forEach((key) => store.removeItem(key));
  } catch {
    /* хранилище недоступно — и удалять нечего */
  }
}
