import { describe, expect, it } from 'vitest';
import { standaloneEngine } from './standaloneEngine';
import { VirtualWebSocket } from './virtualWebSocket';

describe('StandaloneEngine', () => {
  it('возвращает список OAuth-провайдеров', () => {
    const res = standaloneEngine.handleRequest({ id: '1', type: 'auth.oauth.providers' }, () => {});
    expect(res.type).toBe('ok');
    expect(res.payload.providers).toHaveLength(5);
  });

  it('быстрый вход через Google в 1 клик', () => {
    const beginRes = standaloneEngine.handleRequest({ id: '2', type: 'auth.oauth.begin', payload: { provider: 'google' } }, () => {});
    expect(beginRes.payload.authorize_url).toContain('mock_code');

    const cbRes = standaloneEngine.handleRequest({ id: '3', type: 'auth.oauth.callback', payload: { provider: 'google', code: 'mock_code_123' } }, () => {});
    expect(cbRes.type).toBe('ok');
    expect(cbRes.payload.user.email).toBe('user@gmail.com');
    expect(cbRes.payload.token).toBeTruthy();
  });

  it('создает и читает задачи', () => {
    const createRes = standaloneEngine.handleRequest(
      { id: '4', type: 'tasks.create', payload: { title: 'Тестовая задача' } },
      () => {},
    );
    expect(createRes.type).toBe('ok');
    expect(createRes.payload.title).toBe('Тестовая задача');

    const listRes = standaloneEngine.handleRequest({ id: '5', type: 'tasks.list' }, () => {});
    expect(listRes.payload.tasks.some((t: any) => t.title === 'Тестовая задача')).toBe(true);
  });

  it('ИИ-ассистент отвечает на запросы в agent.ask', () => {
    const askRes = standaloneEngine.handleRequest({ id: '6', type: 'agent.ask', payload: { message: 'Привет!' } }, () => {});
    expect(askRes.type).toBe('ok');
    expect(askRes.payload.reply).toContain('Selfvoy');
  });
});

describe('VirtualWebSocket', () => {
  it('подключается мгновенно и отвечает на сообщения', async () => {
    const ws = new VirtualWebSocket('selfvoy://standalone');
    await new Promise<void>((resolve) => {
      ws.onopen = () => resolve();
    });
    expect(ws.readyState).toBe(VirtualWebSocket.OPEN);

    const received = await new Promise<any>((resolve) => {
      ws.onmessage = (ev) => resolve(JSON.parse(ev.data));
      ws.send(JSON.stringify({ id: 'ping-1', type: 'ping' }));
    });
    expect(received.id).toBe('ping-1');
    expect(received.type).toBe('ok');
    expect(received.payload.mode).toBe('standalone');
  });
});
