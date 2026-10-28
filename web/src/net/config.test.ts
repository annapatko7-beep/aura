import { describe, expect, it } from 'vitest';
import { isLocalHost, normalizeServerUrl, platformDefault, sameServers, serverCandidates, type ServerEnv } from './config';

const web: ServerEnv = { platform: 'web', location: { protocol: 'http:', hostname: 'localhost', host: 'localhost:5173' } };

describe('normalizeServerUrl', () => {
  it('голый IP — ws:// и порт сервера по умолчанию', () => {
    expect(normalizeServerUrl('192.168.1.5')).toBe('ws://192.168.1.5:9000');
    expect(normalizeServerUrl('  192.168.1.5:9100 ')).toBe('ws://192.168.1.5:9100');
  });
  it('http(s) превращается в ws(s), лишний слэш убирается', () => {
    expect(normalizeServerUrl('https://selfvoy.example.com/')).toBe('wss://selfvoy.example.com');
    expect(normalizeServerUrl('http://10.0.2.2:9000')).toBe('ws://10.0.2.2:9000');
    expect(normalizeServerUrl('wss://selfvoy.example.com/ws')).toBe('wss://selfvoy.example.com/ws');
  });
  it('голый публичный хост — wss:// без порта (не открытым текстом)', () => {
    expect(normalizeServerUrl('selfvoy.example.com')).toBe('wss://selfvoy.example.com');
    expect(normalizeServerUrl('selfvoy.example.com/ws')).toBe('wss://selfvoy.example.com/ws');
    expect(normalizeServerUrl('selfvoy.example.com:8443')).toBe('wss://selfvoy.example.com:8443');
    expect(normalizeServerUrl('8.8.8.8')).toBe('wss://8.8.8.8');
    expect(normalizeServerUrl('172.32.0.1')).toBe('wss://172.32.0.1');
  });
  it('голый локальный/частный хост — ws:// и порт сервера', () => {
    expect(normalizeServerUrl('localhost')).toBe('ws://localhost:9000');
    expect(normalizeServerUrl('127.0.0.1')).toBe('ws://127.0.0.1:9000');
    expect(normalizeServerUrl('10.0.2.2')).toBe('ws://10.0.2.2:9000');
    expect(normalizeServerUrl('172.20.1.1')).toBe('ws://172.20.1.1:9000');
    expect(normalizeServerUrl('selfvoy.local:9100')).toBe('ws://selfvoy.local:9100');
    expect(normalizeServerUrl('dev.localhost')).toBe('ws://dev.localhost:9000');
    expect(normalizeServerUrl('192.168.1.5/ws')).toBe('ws://192.168.1.5:9000/ws');
  });
  it('явная схема ws:// остаётся как есть, даже для публичного хоста', () => {
    expect(normalizeServerUrl('ws://selfvoy.example.com:9000')).toBe('ws://selfvoy.example.com:9000');
    expect(normalizeServerUrl('http://selfvoy.example.com')).toBe('ws://selfvoy.example.com');
  });
  it('мусор и чужие схемы отклоняются', () => {
    expect(normalizeServerUrl('')).toBeNull();
    expect(normalizeServerUrl('ftp://host')).toBeNull();
    expect(normalizeServerUrl('ws://')).toBeNull();
  });
});

describe('isLocalHost', () => {
  it('loopback, частные сети, .local/.localhost, имя без точки', () => {
    for (const host of ['localhost', 'LOCALHOST', 'a.localhost', 'printer.local', 'server', '127.0.0.1', '127.8.8.8', '10.1.2.3', '172.16.0.1', '172.31.255.255', '192.168.0.1', '169.254.1.1', '[::1]', 'fd00::1', 'fe80::1']) {
      expect(isLocalHost(host), host).toBe(true);
    }
  });
  it('публичные имена и адреса', () => {
    for (const host of ['selfvoy.example.com', 'example.local.com', '8.8.8.8', '172.15.0.1', '172.32.0.1', '192.169.0.1', '11.0.0.1', '2001:db8::1', '']) {
      expect(isLocalHost(host), host).toBe(false);
    }
  });
});

describe('platformDefault', () => {
  it('Android-эмулятор ходит на компьютер разработчика через 10.0.2.2', () => {
    expect(platformDefault({ platform: 'android' })).toBe('ws://10.0.2.2:9000');
  });
  it('веб на настоящем https-хосте — wss того же хоста', () => {
    expect(platformDefault({ platform: 'web', location: { protocol: 'https:', hostname: 'selfvoy.example.com', host: 'selfvoy.example.com' } })).toBe(
      'wss://selfvoy.example.com',
    );
  });
  it('локальная разработка и Electron — 127.0.0.1, не localhost', () => {
    expect(platformDefault(web)).toBe('ws://127.0.0.1:9000');
    expect(platformDefault({ platform: 'electron', location: { protocol: 'file:', hostname: '', host: '' } })).toBe('ws://127.0.0.1:9000');
  });
});

describe('serverCandidates', () => {
  it('продакшн: основной → запасной, без платформенного дефолта', () => {
    expect(serverCandidates(null, { ...web, primary: 'wss://a.example', fallback: 'wss://b.example' })).toEqual(['wss://a.example', 'wss://b.example']);
    expect(serverCandidates(null, { ...web, primary: 'wss://a.example' })).toEqual(['wss://a.example']);
    // Android-сборка с адресом сервера не уходит на 10.0.2.2 без TLS.
    expect(serverCandidates(null, { platform: 'android', primary: 'wss://a.example' })).toEqual(['wss://a.example']);
  });
  it('dev-сборка: к заданным адресам добавляется дефолт, без повторов', () => {
    expect(serverCandidates(null, { ...web, dev: true, primary: 'wss://a.example', fallback: 'wss://b.example' })).toEqual([
      'wss://a.example',
      'wss://b.example',
      'ws://127.0.0.1:9000',
    ]);
    expect(serverCandidates(null, { ...web, dev: true, primary: '127.0.0.1' })).toEqual(['ws://127.0.0.1:9000']);
  });
  it('адрес, заданный пользователем, — единственный', () => {
    expect(serverCandidates('192.168.0.10', { ...web, primary: 'wss://a.example' })).toEqual(['ws://192.168.0.10:9000']);
    expect(serverCandidates('192.168.0.10', { ...web, dev: true, primary: 'wss://a.example' })).toEqual(['ws://192.168.0.10:9000']);
  });
  it('неразборчивый адрес пользователя не ломает список', () => {
    expect(serverCandidates('ftp://x', { ...web, primary: 'wss://a.example' })).toEqual(['wss://a.example']);
  });
  it('без переменных сборки остаётся платформенный дефолт (разработка)', () => {
    expect(serverCandidates(null, { platform: 'android' })).toEqual(['ws://10.0.2.2:9000']);
    expect(serverCandidates(null, { platform: 'android', dev: true })).toEqual(['ws://10.0.2.2:9000']);
    expect(serverCandidates(null, web)).toEqual(['ws://127.0.0.1:9000']);
  });
  it('неразборчивые переменные сборки — как будто их нет', () => {
    expect(serverCandidates(null, { ...web, primary: 'ftp://x' })).toEqual(['ws://127.0.0.1:9000']);
  });
});

describe('sameServers', () => {
  it('сравнивает наборы адресов по порядку', () => {
    expect(sameServers(['ws://a'], ['ws://a'])).toBe(true);
    expect(sameServers(['ws://a'], ['ws://b'])).toBe(false);
    expect(sameServers(['ws://a', 'ws://b'], ['ws://a'])).toBe(false);
    // Пользователь ввёл тот же адрес, что задан при сборке, — сервер тот же.
    expect(sameServers(serverCandidates('wss://a.example', web), serverCandidates(null, { ...web, primary: 'wss://a.example' }))).toBe(true);
  });
});
