/*
  Адрес сервера Selfvoy (WebSocket). Порядок выбора:

  1. адрес, который пользователь ввёл сам («Адрес сервера» на экране входа
     или в настройках) — хранится на устройстве;
  2. VITE_SELFVOY_SERVER_URL — задаётся при сборке (.env или переменная CI),
     для продакшна это wss://… за TLS-прокси;
  3. VITE_SELFVOY_FALLBACK_URL — запасной адрес: если основной не отвечает,
     клиент сам переключается на него;
  4. платформенный дефолт — только если адрес при сборке не задан (или это
     dev-сборка Vite): на Android-эмуляторе компьютер разработчика — это
     10.0.2.2 (localhost там — сам телефон), в браузере, открытом с настоящего
     хоста по https, — тот же хост по wss, иначе ws://127.0.0.1:9000.

  В продакшн-сборке с VITE_SELFVOY_SERVER_URL клиент никогда не уходит на
  платформенный дефолт: иначе токены могли бы уехать на ws:// без TLS или на
  чужой локальный порт.

  Реальному телефону localhost/10.0.2.2 не подходят: нужен IP компьютера в
  той же Wi-Fi сети (ws://192.168.x.x:9000) или публичный wss-адрес.
*/
import { Capacitor } from '@capacitor/core';

const OVERRIDE_KEY = 'selfvoy.server';
export const DEV_PORT = 9000;

export interface ServerEnv {
  primary?: string;
  fallback?: string;
  platform: string;
  /** Dev-сборка Vite: к заданным адресам добавляется платформенный дефолт. */
  dev?: boolean;
  location?: { protocol: string; hostname: string; host: string };
}

/**
 * Локальный или частный адрес: localhost, *.localhost, *.local, имя без точки
 * (хост в своей сети / docker), loopback 127/8, частные 10/8, 172.16/12,
 * 192.168/16, link-local 169.254/16 и такие же IPv6 (::1, fc00::/7, fe80::/10).
 * Только сюда разумно ходить без TLS.
 */
export function isLocalHost(hostname: string): boolean {
  const host = hostname.trim().toLowerCase().replace(/^\[|\]$/g, '').replace(/\.$/, '');
  if (!host) return false;
  if (host === 'localhost' || host.endsWith('.localhost') || host.endsWith('.local')) return true;
  if (host.includes(':')) {
    // IPv6
    return host === '::1' || host === '::' || /^f[cd][0-9a-f]{0,2}:/.test(host) || /^fe[89ab][0-9a-f]?:/.test(host);
  }
  const ip = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
  if (ip) {
    const [a, b] = [Number(ip[1]), Number(ip[2])];
    return a === 127 || a === 10 || a === 0 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254);
  }
  return !host.includes('.');
}

/**
 * Приводит ввод пользователя к ws(s)-адресу: «192.168.1.5» → ws://192.168.1.5:9000,
 * «https://selfvoy.example.com» → wss://selfvoy.example.com. Голый публичный хост
 * («selfvoy.example.com») — wss:// без порта: пароли и токены не должны идти открытым
 * текстом; голый локальный/частный хост — ws:// и порт сервера разработки.
 * Явный ws:// оставляем как ввели. Неразборчивый ввод → null.
 */
export function normalizeServerUrl(input: string): string | null {
  let value = input.trim();
  if (!value) return null;
  if (value === 'selfvoy://standalone' || value === 'ws://standalone' || value === 'standalone' || value === 'local') {
    return 'selfvoy://standalone';
  }
  if (/^https?:\/\//i.test(value)) value = value.replace(/^http/i, 'ws');
  else if (/^[a-z][a-z0-9+.-]*:\/\//i.test(value) && !/^wss?:\/\//i.test(value)) return null; // ftp://, file://…
  if (!/^wss?:\/\//i.test(value)) {
    const authority = value.split(/[/?#]/)[0];
    const hasPort = /:\d+$/.test(authority);
    let hostname = '';
    try {
      hostname = new URL(`ws://${authority}`).hostname;
    } catch {
      return null;
    }
    const rest = value.slice(authority.length);
    value = isLocalHost(hostname) ? `ws://${authority}${hasPort ? '' : `:${DEV_PORT}`}${rest}` : `wss://${value}`;
  }
  try {
    const url = new URL(value);
    if (url.protocol !== 'ws:' && url.protocol !== 'wss:') return null;
    if (!url.hostname) return null;
    // URL добавляет «/» к пустому пути — для наглядности убираем его.
    return url.toString().replace(/\/$/, '');
  } catch {
    return null;
  }
}

/** Платформенный адрес по умолчанию (без пользовательских настроек и переменных сборки). */
export function platformDefault(env: ServerEnv): string {
  if (env.platform === 'android') return `ws://10.0.2.2:${DEV_PORT}`;
  const loc = env.location;
  if (loc && loc.protocol === 'https:' && loc.hostname && !['localhost', '127.0.0.1'].includes(loc.hostname)) {
    // Веб-версия на настоящем хосте: сервер за тем же TLS-прокси.
    return `wss://${loc.host}`;
  }
  return `ws://127.0.0.1:${DEV_PORT}`;
}

/** Один и тот же набор адресов (смена адреса без смены сервера не требует нового входа). */
export function sameServers(a: string[], b: string[]): boolean {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

/** Кандидаты по порядку; без повторов и без пустых значений. */
export function serverCandidates(override: string | null, env: ServerEnv): string[] {
  // Если пользователь задал адрес сам — подключаемся только к нему: молча уходить
  // на другой сервер было бы сюрпризом. Неразборчивый адрес игнорируется.
  const own = override ? normalizeServerUrl(override) : null;
  if (own) return [own];
  const list = [env.primary, env.fallback]
    .map((v) => (v ? normalizeServerUrl(v) : null))
    .filter((v): v is string => Boolean(v));
  // Платформенный дефолт — только для разработки: когда адрес при сборке не задан
  // или это dev-сборка. В продакшне с заданным адресом запасной путь на
  // ws://127.0.0.1 / ws://10.0.2.2 означал бы токены без TLS на чужом порту.
  if (!list.length || env.dev) list.push(platformDefault(env));
  return [...new Set(list)];
}

export function currentEnv(): ServerEnv {
  return {
    primary: import.meta.env.VITE_SELFVOY_SERVER_URL,
    fallback: import.meta.env.VITE_SELFVOY_FALLBACK_URL,
    dev: Boolean(import.meta.env.DEV),
    platform: Capacitor.getPlatform(),
    location: typeof location === 'undefined' ? undefined : { protocol: location.protocol, hostname: location.hostname, host: location.host },
  };
}

export function loadOverride(): string | null {
  try {
    return localStorage.getItem(OVERRIDE_KEY);
  } catch {
    return null;
  }
}

export function saveOverride(url: string | null): void {
  try {
    if (url) localStorage.setItem(OVERRIDE_KEY, url);
    else localStorage.removeItem(OVERRIDE_KEY);
  } catch {
    /* хранилище недоступно — адрес действует до перезапуска */
  }
}
