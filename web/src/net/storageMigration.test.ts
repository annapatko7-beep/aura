import { describe, expect, it } from 'vitest';
import { migrateStorageKeys } from './storageMigration';

function store(entries: Record<string, string>) {
  const data = new Map(Object.entries(entries));
  return {
    data,
    get length() {
      return data.size;
    },
    key: (i: number) => [...data.keys()][i] ?? null,
    getItem: (k: string) => data.get(k) ?? null,
    setItem: (k: string, v: string) => void data.set(k, v),
    removeItem: (k: string) => void data.delete(k),
  };
}

describe('migrateStorageKeys', () => {
  it('переносит старые ключи на новые и удаляет старые — вход сохраняется', () => {
    const s = store({
      'aura.secure.access': 'jwt',
      'aura.secure.refresh': 'r1',
      'aura.server': 'wss://selfvoy.example.com',
      'aura.dialog.5': '[]',
      other: 'x',
    });
    expect(migrateStorageKeys(s)).toBe(4);
    expect(Object.fromEntries(s.data)).toEqual({
      'selfvoy.secure.access': 'jwt',
      'selfvoy.secure.refresh': 'r1',
      'selfvoy.server': 'wss://selfvoy.example.com',
      'selfvoy.dialog.5': '[]',
      other: 'x',
    });
  });

  it('не перезаписывает уже существующий новый ключ, но старый удаляет', () => {
    const s = store({ 'aura.server': 'ws://old', 'selfvoy.server': 'ws://new' });
    migrateStorageKeys(s);
    expect(Object.fromEntries(s.data)).toEqual({ 'selfvoy.server': 'ws://new' });
  });

  it('повторный запуск ничего не меняет; без хранилища — не бросает', () => {
    const s = store({ 'selfvoy.user': '{}' });
    expect(migrateStorageKeys(s)).toBe(0);
    expect(Object.fromEntries(s.data)).toEqual({ 'selfvoy.user': '{}' });
    expect(migrateStorageKeys(null)).toBe(0);
  });

  it('недоступное хранилище — не бросает', () => {
    const broken = {
      get length(): number {
        throw new Error('SecurityError');
      },
      key: () => null,
      getItem: () => null,
      setItem: () => undefined,
      removeItem: () => undefined,
    };
    expect(migrateStorageKeys(broken)).toBe(0);
  });
});
