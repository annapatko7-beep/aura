import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

/** Простой localStorage в памяти; broken — как в приватном режиме (любой доступ бросает). */
function fakeStorage(broken = false): Storage {
  const data = new Map<string, string>();
  const guard = () => {
    if (broken) throw new DOMException('denied', 'SecurityError');
  };
  return {
    get length() {
      guard();
      return data.size;
    },
    key: (i) => (guard(), [...data.keys()][i] ?? null),
    getItem: (k) => (guard(), data.get(k) ?? null),
    setItem: (k, v) => (guard(), void data.set(k, String(v))),
    removeItem: (k) => (guard(), void data.delete(k)),
    clear: () => (guard(), data.clear()),
  };
}

async function freshModule() {
  vi.resetModules();
  return import('./secureStore');
}

beforeEach(() => vi.unstubAllGlobals());
afterEach(() => vi.unstubAllGlobals());

describe('deviceInfo', () => {
  it('без хранилища — случайный id на время запуска, не общий «web-ephemeral», и не постоянный', async () => {
    vi.stubGlobal('localStorage', fakeStorage(true));
    const a = await freshModule();
    const first = await a.deviceInfo();
    expect(first.id).not.toBe('web-ephemeral');
    expect(first.id).toMatch(/^[A-Za-z0-9-]{16,64}$/);
    expect(first.persistent).toBe(false);
    // В пределах запуска id стабилен.
    expect((await a.deviceInfo()).id).toBe(first.id);
    // Другой запуск (другая вкладка) — другой id.
    const b = await freshModule();
    expect((await b.deviceInfo()).id).not.toBe(first.id);
  });

  it('с хранилищем — постоянный id, переживает перезапуск', async () => {
    const storage = fakeStorage();
    vi.stubGlobal('localStorage', storage);
    const a = await freshModule();
    const first = await a.deviceInfo();
    expect(first.persistent).toBe(true);
    const b = await freshModule();
    expect(await b.deviceInfo()).toEqual(first);
  });

  it('id старой версии клиента переносится', async () => {
    const storage = fakeStorage();
    storage.setItem('selfvoy.deviceId', '0d7f9a3c-1111-4222-8333-944455556666');
    vi.stubGlobal('localStorage', storage);
    const mod = await freshModule();
    expect((await mod.deviceInfo()).id).toBe('0d7f9a3c-1111-4222-8333-944455556666');
  });

  it('мусор в хранилище заменяется новым id', async () => {
    const storage = fakeStorage();
    storage.setItem('selfvoy.secure.device', 'web-ephemeral');
    vi.stubGlobal('localStorage', storage);
    const mod = await freshModule();
    expect((await mod.deviceInfo()).id).not.toBe('web-ephemeral');
  });
});

describe('токены', () => {
  it('без localStorage токены живут в памяти до перезапуска', async () => {
    vi.stubGlobal('localStorage', fakeStorage(true));
    const mod = await freshModule();
    await mod.saveTokens({ access: 'a', refresh: 'r' });
    expect(await mod.loadTokens()).toEqual({ access: 'a', refresh: 'r' });
    await mod.clearTokens();
    expect(await mod.loadTokens()).toBeNull();
  });

  it('выход не трогает id устройства', async () => {
    vi.stubGlobal('localStorage', fakeStorage());
    const mod = await freshModule();
    const device = await mod.deviceInfo();
    await mod.saveTokens({ access: 'a', refresh: 'r' });
    await mod.clearTokens();
    const again = await freshModule();
    expect((await again.deviceInfo()).id).toBe(device.id);
  });
});
