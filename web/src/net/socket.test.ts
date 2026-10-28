import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SelfvoyError, SelfvoySocket, isNetworkError } from './socket';

/** Поддельный WebSocket: тест сам открывает, закрывает и отвечает. */
class FakeWs {
  static all: FakeWs[] = [];
  readyState = 0;
  sent: { id: string; type: string; payload: Record<string, unknown> }[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((m: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  constructor(public url: string) {
    FakeWs.all.push(this);
  }
  send(data: string) {
    this.sent.push(JSON.parse(data));
  }
  close() {
    if (this.readyState === 3) return;
    this.readyState = 3;
    this.onclose?.();
  }
  open() {
    this.readyState = 1;
    this.onopen?.();
  }
  reply(message: Record<string, unknown>) {
    this.onmessage?.({ data: JSON.stringify(message) });
  }
  last() {
    return this.sent[this.sent.length - 1];
  }
}

const latest = () => FakeWs.all[FakeWs.all.length - 1];

function make(candidates = ['ws://a', 'ws://b'], onOpen?: (s: SelfvoySocket) => Promise<void>) {
  return new SelfvoySocket({
    candidates: () => candidates,
    onOpen,
    connectTimeoutMs: 1000,
    requestTimeoutMs: 500,
    backoff: [100, 200],
    WebSocketImpl: FakeWs as unknown as new (url: string) => WebSocket,
  });
}

beforeEach(() => {
  FakeWs.all = [];
  vi.useFakeTimers();
});
afterEach(() => vi.useRealTimers());

describe('SelfvoySocket', () => {
  it('запрос до открытия ждёт соединения; ответ сопоставляется по id', async () => {
    const socket = make();
    socket.start();
    const answer = socket.request<{ user: string }>('auth.me');
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    const sent = latest().last();
    expect(sent).toMatchObject({ type: 'auth.me', payload: {} });
    latest().reply({ id: sent.id, type: 'ok', payload: { user: 'alex' } });
    await expect(answer).resolves.toEqual({ user: 'alex' });
  });

  it('ошибка сервера — SelfvoyError с кодом', async () => {
    const socket = make();
    socket.start();
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    const answer = socket.request('auth.login', { email: 'x' });
    latest().reply({ id: latest().last().id, type: 'error', code: 'requires_2fa', message: 'Нужен код' });
    const error = await answer.catch((e) => e);
    expect(error).toBeInstanceOf(SelfvoyError);
    expect(error.code).toBe('requires_2fa');
    expect(isNetworkError(error)).toBe(false);
  });

  it('нет ответа — таймаут, это сетевая ошибка', async () => {
    const socket = make();
    socket.start();
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    const answer = socket.request('tasks.list').catch((e) => e);
    await vi.advanceTimersByTimeAsync(600);
    const error = await answer;
    expect(error.code).toBe('timeout');
    expect(isNetworkError(error)).toBe(true);
  });

  it('onOpen (авторизация) идёт раньше запросов интерфейса', async () => {
    const order: string[] = [];
    const socket = make(['ws://a'], async (s) => {
      const auth = s.requestNow('auth.token', { token: 't' });
      order.push('auth');
      latest().reply({ id: latest().last().id, type: 'ok', payload: {} });
      await auth;
    });
    socket.start();
    const ui = socket.request('tasks.list');
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    order.push(latest().last().type);
    expect(order).toEqual(['auth', 'tasks.list']);
    expect(latest().sent.map((m) => m.type)).toEqual(['auth.token', 'tasks.list']);
    latest().reply({ id: latest().last().id, type: 'ok', payload: {} });
    await ui;
  });

  it('недоступный адрес — следующий кандидат, затем пауза; состояние offline', async () => {
    const states: string[] = [];
    const socket = make();
    socket.onState((s) => states.push(s));
    socket.start();
    expect(latest().url).toBe('ws://a');
    latest().close();
    expect(socket.state).toBe('offline');
    expect(socket.lastError).toContain('ws://a');
    await vi.advanceTimersByTimeAsync(250);
    expect(latest().url).toBe('ws://b');
    latest().close();
    await vi.advanceTimersByTimeAsync(99);
    expect(FakeWs.all).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(1);
    expect(latest().url).toBe('ws://a');
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    expect(socket.state).toBe('open');
    expect(states).toEqual(['connecting', 'offline', 'connecting', 'offline', 'connecting', 'open']);
  });

  it('сервер молчит при подключении — таймаут подключения', async () => {
    const socket = make(['ws://a']);
    socket.start();
    await vi.advanceTimersByTimeAsync(1000);
    expect(socket.state).toBe('offline');
    expect(socket.lastError).toContain('не отвечает');
  });

  it('обрыв связи отклоняет ждущие запросы и переподключается', async () => {
    const socket = make(['ws://a']);
    socket.start();
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    const answer = socket.request('chat.list').catch((e) => e);
    latest().close();
    expect((await answer).code).toBe('disconnected');
    await vi.advanceTimersByTimeAsync(100);
    expect(FakeWs.all).toHaveLength(2);
  });

  it('retryNow на экране ожидания пробует сразу, stop останавливает попытки', async () => {
    const socket = make(['ws://a']);
    socket.start();
    latest().close();
    socket.retryNow();
    expect(FakeWs.all).toHaveLength(2);
    socket.stop();
    expect(socket.state).toBe('closed');
    await vi.advanceTimersByTimeAsync(5000);
    expect(FakeWs.all).toHaveLength(2);
  });

  it('сеть отказала во время onOpen — переподключение, а не «открыто без входа»', async () => {
    let calls = 0;
    const socket = make(['ws://a'], async (s) => {
      calls += 1;
      if (calls === 1) await s.requestNow('auth.token', { token: 't' }, 100);
    });
    socket.start();
    latest().open();
    await vi.advanceTimersByTimeAsync(150);
    expect(socket.state).toBe('offline');
    expect(socket.lastError).toContain('авторизацию');
    await vi.advanceTimersByTimeAsync(100);
    expect(FakeWs.all).toHaveLength(2);
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    expect(socket.state).toBe('open');
    expect(calls).toBe(2);
  });

  it('прочая ошибка onOpen не мешает соединению', async () => {
    const socket = make(['ws://a'], async () => {
      throw new SelfvoyError('unauthorized', 'нет');
    });
    socket.start();
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    expect(socket.state).toBe('open');
  });

  it('события доставляются подписчикам', async () => {
    const socket = make(['ws://a']);
    const seen: string[] = [];
    socket.onEvent((e) => seen.push(`${e.event}:${(e.payload as { body?: string }).body}`));
    socket.start();
    latest().open();
    await vi.advanceTimersByTimeAsync(0);
    latest().reply({ type: 'event', event: 'chat.message', payload: { body: 'привет' } });
    expect(seen).toEqual(['chat.message:привет']);
  });
});
