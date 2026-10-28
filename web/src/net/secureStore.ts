/*
  Хранение токенов (access JWT + refresh).

  - Desktop (Electron): шифрование ОС через safeStorage (Keychain / DPAPI /
    libsecret) — preload отдаёт window.selfvoySecure, токены лежат в userData
    зашифрованными.
  - Телефон (Capacitor) и браузер: localStorage WebView — он изолирован в
    песочнице приложения; на рутованном устройстве или в общем браузере это
    слабее Keychain/Keystore (см. web/README.md).

  Пароль не хранится нигде и никогда.
*/

export interface SecureBridge {
  get(key: string): Promise<string | null>;
  set(key: string, value: string): Promise<void>;
  remove(key: string): Promise<void>;
  /** true, если ОС действительно шифрует значения. */
  encrypted(): Promise<boolean>;
}

declare global {
  interface Window {
    selfvoySecure?: SecureBridge;
  }
}

const PREFIX = 'selfvoy.secure.';

/**
 * Запасное хранилище в памяти: localStorage недоступен (приватный режим,
 * запрет сайта) — значения живут до перезапуска, а не теряются сразу.
 */
const memory = new Map<string, string>();

/** Работает ли localStorage на самом деле (запись и чтение, а не только наличие). */
export function localStorageWorks(): boolean {
  try {
    const probe = `${PREFIX}probe`;
    localStorage.setItem(probe, '1');
    const ok = localStorage.getItem(probe) === '1';
    localStorage.removeItem(probe);
    return ok;
  } catch {
    return false;
  }
}

const localBridge: SecureBridge = {
  async get(key) {
    try {
      const value = localStorage.getItem(PREFIX + key);
      if (value !== null) return value;
    } catch {
      /* хранилище недоступно — смотрим в память */
    }
    return memory.get(key) ?? null;
  },
  async set(key, value) {
    try {
      localStorage.setItem(PREFIX + key, value);
      memory.delete(key);
    } catch {
      // Хранилище недоступно (приватный режим) — значение живёт до перезапуска.
      memory.set(key, value);
    }
  },
  async remove(key) {
    memory.delete(key);
    try {
      localStorage.removeItem(PREFIX + key);
    } catch {
      /* нечего удалять */
    }
  },
  async encrypted() {
    return false;
  },
};

export const secureStore: SecureBridge = typeof window !== 'undefined' && window.selfvoySecure ? window.selfvoySecure : localBridge;

export interface Tokens {
  access: string;
  refresh: string;
}

export async function loadTokens(): Promise<Tokens | null> {
  const [access, refresh] = await Promise.all([secureStore.get('access'), secureStore.get('refresh')]);
  if (!access && !refresh) return null;
  return { access: access ?? '', refresh: refresh ?? '' };
}

export async function saveTokens(tokens: Tokens): Promise<void> {
  await Promise.all([secureStore.set('access', tokens.access), secureStore.set('refresh', tokens.refresh)]);
}

export async function clearTokens(): Promise<void> {
  await Promise.all([secureStore.remove('access'), secureStore.remove('refresh')]);
}

export interface DeviceInfo {
  /** Id устройства для «доверенных устройств» 2FA. Не секрет, но и не общий для всех. */
  id: string;
  /** Id переживёт перезапуск. Если нет — «доверять устройству» бессмысленно и не предлагается. */
  persistent: boolean;
}

const DEVICE_KEY = 'device';
const LEGACY_DEVICE_KEY = 'selfvoy.deviceId';
const DEVICE_RE = /^[A-Za-z0-9-]{16,64}$/;

function randomId(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') return crypto.randomUUID();
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

let device: Promise<DeviceInfo> | null = null;

async function loadDevice(): Promise<DeviceInfo> {
  // Desktop хранит id в зашифрованном хранилище; телефон и браузер — в localStorage.
  const persistentStore = secureStore !== localBridge || localStorageWorks();
  let id: string | null = null;
  try {
    id = await secureStore.get(DEVICE_KEY);
  } catch {
    id = null;
  }
  if (!id || !DEVICE_RE.test(id)) {
    // Старые версии клиента держали id в localStorage под своим ключом.
    let legacy: string | null = null;
    try {
      legacy = localStorage.getItem(LEGACY_DEVICE_KEY);
    } catch {
      legacy = null;
    }
    id = legacy && DEVICE_RE.test(legacy) ? legacy : randomId();
    try {
      await secureStore.set(DEVICE_KEY, id);
    } catch {
      // Не сохранить — id живёт до перезапуска (и устройство не будет «доверенным»).
      return { id, persistent: false };
    }
  }
  let stored: string | null = null;
  try {
    stored = await secureStore.get(DEVICE_KEY);
  } catch {
    stored = null;
  }
  return { id, persistent: persistentStore && stored === id };
}

/**
 * Id этого устройства. Никогда не общий для всех: без хранилища — случайный на
 * время запуска (раньше был постоянный «web-ephemeral», и доверие к нему
 * снимало 2FA у любого, кто пришлёт тот же id).
 */
export function deviceInfo(): Promise<DeviceInfo> {
  if (!device) device = loadDevice();
  return device;
}
