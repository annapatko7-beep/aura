import { describe, expect, it } from 'vitest';
import { clearUserCaches, dialogKey, negotiationsKey, USER_KEY } from './userCache';

function store(entries: Record<string, string>) {
  const data = new Map(Object.entries(entries));
  return {
    data,
    get length() {
      return data.size;
    },
    key: (i: number) => [...data.keys()][i] ?? null,
    removeItem: (k: string) => void data.delete(k),
  };
}

describe('clearUserCaches', () => {
  it('удаляет профиль и ленты всех пользователей, остальное не трогает', () => {
    const s = store({
      [USER_KEY]: '{}',
      [dialogKey(5)]: '[]',
      [dialogKey(7)]: '[]',
      [negotiationsKey(5)]: '[]',
      'selfvoy.server': 'ws://x',
      'selfvoy.secure.device': 'id',
      'selfvoy.secure.access': 'token',
    });
    clearUserCaches(s);
    expect([...s.data.keys()].sort()).toEqual(['selfvoy.secure.access', 'selfvoy.secure.device', 'selfvoy.server']);
  });

  it('без хранилища — ничего не делает и не бросает', () => {
    expect(() => clearUserCaches(null)).not.toThrow();
  });
});
