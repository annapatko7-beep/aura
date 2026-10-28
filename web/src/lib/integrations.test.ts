import { describe, expect, it } from 'vitest';
import { SelfvoyError } from '../net/socket';
import { icsFeedUrl, isProRequired, planView, webcalUrl } from './integrations';

const PATH = '/ics/AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-abcde.ics';

describe('ссылка подписки на календарь', () => {
  it('готовый url от сервера — как есть', () => {
    expect(icsFeedUrl('https://api.example.com/ics/x.ics', '/ics/x.ics', 'ws://127.0.0.1:9000')).toBe('https://api.example.com/ics/x.ics');
  });

  it('без url — из адреса сервера: wss → https, ws → http, тот же порт', () => {
    expect(icsFeedUrl('', PATH, 'wss://selfvoy.example.com')).toBe(`https://selfvoy.example.com${PATH}`);
    expect(icsFeedUrl('', PATH, 'ws://192.168.1.10:9000')).toBe(`http://192.168.1.10:9000${PATH}`);
    expect(icsFeedUrl('', PATH, 'wss://selfvoy.example.com:8443/ws')).toBe(`https://selfvoy.example.com:8443${PATH}`);
  });

  it('нечего собрать — null', () => {
    expect(icsFeedUrl('', '', 'wss://selfvoy.example.com')).toBeNull();
    expect(icsFeedUrl('', PATH, '')).toBeNull();
    expect(icsFeedUrl('', PATH, 'ftp://x')).toBeNull();
    expect(icsFeedUrl('javascript:alert(1)', 'ics', 'wss://x')).toBeNull();
  });

  it('webcal только для https', () => {
    expect(webcalUrl(`https://selfvoy.example.com${PATH}`)).toBe(`webcal://selfvoy.example.com${PATH}`);
    expect(webcalUrl(`http://192.168.1.10:9000${PATH}`)).toBeNull();
    expect(webcalUrl(null)).toBeNull();
  });
});

describe('тариф', () => {
  const now = new Date('2026-10-16T12:00:00Z');
  it('Pro со сроком, Free в бете и при enforced', () => {
    expect(planView({ name: 'pro', until: '2026-11-12T12:00:00Z', enforced: false }, now)).toMatchObject({ label: 'Pro до 12 ноября', pro: true });
    expect(planView({ name: 'free', until: '', enforced: false }, now)).toMatchObject({ label: 'Free', pro: false, hint: 'Пока идёт бета, функции Pro открыты всем' });
    expect(planView({ name: 'free', until: '', enforced: true }, now)?.hint).toContain('Функции Pro');
    expect(planView({ name: 'pro', until: '2026-10-01T00:00:00Z', enforced: true }, now)?.pro).toBe(false);
    expect(planView(undefined, now)).toBeNull();
  });

  it('pro_required распознаётся по коду', () => {
    expect(isProRequired(new SelfvoyError('pro_required', 'нужен Pro'))).toBe(true);
    expect(isProRequired(new SelfvoyError('bad_request', 'x'))).toBe(false);
    expect(isProRequired(new Error('pro_required'))).toBe(false);
  });
});
