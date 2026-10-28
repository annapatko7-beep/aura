/*
  WebSocket-клиент протокола Selfvoy (docs/PROTOCOL.md).

  Запрос:  {"id", "type": "<домен>.<метод>", "payload": {...}}
  Ответ:   {"id", "type": "ok", "payload"} | {"id", "type": "error", "code", "message"}
  Событие: {"type": "event", "event": "...", "payload": {...}}

  Соединение живёт само: таймаут подключения, переподключение с нарастающей
  паузой, перебор адресов-кандидатов (основной → запасной → дефолт). После
  каждого открытия сначала выполняется onOpen (повторная авторизация токеном),
  и только потом уходят запросы интерфейса — иначе они получили бы отказ
  «нет авторизации».
*/

export type ConnState = 'connecting' | 'open' | 'offline' | 'closed';

import { VirtualWebSocket } from './virtualWebSocket';

export class SelfvoyError extends Error {
  readonly code: string;
  constructor(code: string, message: string) {
    super(message);
    this.name = 'SelfvoyError';
    this.code = code;
  }
}

/** Сетевая проблема (не ответ сервера): повторить позже, а не показывать как ошибку данных. */
export const isNetworkError = (e: unknown) =>
  e instanceof SelfvoyError && ['offline', 'disconnected', 'timeout'].includes(e.code);

export interface SelfvoyEvent {
  event: string;
  payload: Record<string, unknown>;
}

type Payload = Record<string, unknown>;

interface Pending {
  resolve: (payload: Payload) => void;
  reject: (error: SelfvoyError) => void;
  timer: ReturnType<typeof setTimeout>;
}

type WebSocketCtor = new (url: string) => WebSocket;

export interface SocketOptions {
  /** Адреса по порядку (см. net/config.ts); перечитываются при каждой попытке. */
  candidates: () => string[];
  /** Выполняется сразу после открытия, до запросов интерфейса: повторная авторизация. */
  onOpen?: (socket: SelfvoySocket) => Promise<void>;
  connectTimeoutMs?: number;
  requestTimeoutMs?: number;
  /** Паузы между кругами попыток, мс. */
  backoff?: number[];
  WebSocketImpl?: WebSocketCtor;
  fallbackToStandalone?: boolean;
}

const OPEN = 1;

export class SelfvoySocket {
  state: ConnState = 'closed';
  url = '';
  /** Человекочитаемая причина последней неудачи подключения. */
  lastError = '';

  private ws: WebSocket | null = null;
  private seq = 0;
  private pending = new Map<string, Pending>();
  private waiters = new Set<() => void>();
  private eventSubs = new Set<(e: SelfvoyEvent) => void>();
  private stateSubs = new Set<(s: ConnState) => void>();
  private candidate = 0;
  private round = 0;
  private active = false;
  private ready = false;
  private retryTimer: ReturnType<typeof setTimeout> | undefined;
  private connectTimer: ReturnType<typeof setTimeout> | undefined;
  private readonly opts: Required<Omit<SocketOptions, 'onOpen'>> & Pick<SocketOptions, 'onOpen'>;

  constructor(options: SocketOptions) {
    this.opts = {
      fallbackToStandalone: false,
      connectTimeoutMs: 2500,
      requestTimeoutMs: 25000,
      backoff: [1000, 2000, 4000, 8000, 15000],
      WebSocketImpl: (globalThis as unknown as { WebSocket: WebSocketCtor }).WebSocket,
      ...options,
    };
  }

  start(): void {
    if (this.active) return;
    this.active = true;
    this.open();
  }

  stop(): void {
    this.active = false;
    clearTimeout(this.retryTimer);
    clearTimeout(this.connectTimer);
    const ws = this.ws;
    this.ws = null;
    ws?.close();
    this.dropPending('Соединение закрыто');
    this.setState('closed');
  }

  /** «Повторить» на экране ожидания: сразу новая попытка с первого адреса. */
  retryNow(): void {
    this.candidate = 0;
    this.round = 0;
    if (this.state === 'open') return;
    this.active = true;
    this.open();
  }

  /** Адрес сменился (настройки) — закрыть текущее соединение и подключиться заново. */
  reconnect(): void {
    const ws = this.ws;
    this.ws = null;
    ws?.close();
    this.dropPending('Переподключение к другому серверу');
    this.candidate = 0;
    this.round = 0;
    this.active = true;
    this.open();
  }

  /** Запрос к серверу. До готовности соединения ждёт его (в пределах таймаута). */
  async request<T = Payload>(type: string, payload: Payload = {}, timeoutMs?: number): Promise<T> {
    const timeout = timeoutMs ?? this.opts.requestTimeoutMs;
    if (!this.ready) await this.waitReady(timeout);
    return this.send<T>(type, payload, timeout);
  }

  /** Запрос в обход ожидания готовности — только для onOpen (авторизация при открытии). */
  requestNow<T = Payload>(type: string, payload: Payload = {}, timeoutMs?: number): Promise<T> {
    return this.send<T>(type, payload, timeoutMs ?? this.opts.requestTimeoutMs);
  }

  onEvent(fn: (e: SelfvoyEvent) => void): () => void {
    this.eventSubs.add(fn);
    return () => this.eventSubs.delete(fn);
  }

  onState(fn: (s: ConnState) => void): () => void {
    this.stateSubs.add(fn);
    return () => this.stateSubs.delete(fn);
  }

  // ------------------------------------------------------------------ internals

  private open(): void {
    clearTimeout(this.retryTimer);
    clearTimeout(this.connectTimer);
    const list = this.opts.candidates();
    if (!list.length) {
      this.fail('Не задан адрес сервера');
      return;
    }
    this.url = list[this.candidate % list.length];
    this.ready = false;
    this.setState('connecting');

    let ws: WebSocket;
    try {
      if (this.url.startsWith('selfvoy:') || this.url.includes('standalone')) {
        ws = new VirtualWebSocket(this.url) as unknown as WebSocket;
      } else {
        ws = new this.opts.WebSocketImpl(this.url);
      }
    } catch {
      this.fail(`Неверный адрес сервера: ${this.url}`);
      return;
    }
    this.ws = ws;
    this.connectTimer = setTimeout(() => {
      if (this.ws === ws && ws.readyState !== OPEN) {
        this.ws = null;
        ws.close();
        this.fail(`Сервер ${this.url} не отвечает`);
      }
    }, this.opts.connectTimeoutMs);

    ws.onopen = async () => {
      if (this.ws !== ws) return;
      clearTimeout(this.connectTimer);
      try {
        await this.opts.onOpen?.(this);
      } catch (error) {
        if (this.ws !== ws) return;
        if (isNetworkError(error)) {
          // Сервер не ответил на авторизацию — соединение негодное: без этого
          // приложение осталось бы «открытым», но без входа (вечная заставка).
          this.ws = null;
          ws.close();
          this.dropPending('Сервер не ответил на авторизацию');
          this.fail(`Сервер ${this.url} не ответил на авторизацию`);
          return;
        }
        // Прочие ошибки авторизации — соединение рабочее (вход заново).
      }
      if (this.ws !== ws) return;
      this.round = 0;
      this.lastError = '';
      this.ready = true;
      this.setState('open');
      const waiters = [...this.waiters];
      this.waiters.clear();
      waiters.forEach((w) => w());
    };
    ws.onmessage = (message) => this.handle(String(message.data));
    ws.onclose = () => {
      if (this.ws !== ws) return;
      this.ws = null;
      this.dropPending('Связь с сервером прервалась');
      this.fail(this.state === 'open' ? 'Связь с сервером прервалась' : `Не удалось подключиться к ${this.url}`);
    };
    ws.onerror = () => {
      /* за ошибкой всегда следует onclose */
    };
  }

  private fail(reason: string): void {
    clearTimeout(this.connectTimer);
    this.ready = false;
    this.lastError = reason;
    this.setState('offline');
    if (!this.active) return;
    // Следующий кандидат — почти сразу; прошли весь список — пауза по нарастающей.
    const total = Math.max(1, this.opts.candidates().length);
    this.candidate = (this.candidate + 1) % total;
    let delay = 250;
    if (this.candidate === 0) {
      delay = this.opts.backoff[Math.min(this.round, this.opts.backoff.length - 1)];
      this.round += 1;
    }
    this.retryTimer = setTimeout(() => this.open(), delay);
  }

  private send<T>(type: string, payload: Payload, timeout: number): Promise<T> {
    const ws = this.ws;
    if (!ws || ws.readyState !== OPEN) {
      return Promise.reject(new SelfvoyError('offline', 'Нет связи с сервером Selfvoy'));
    }
    const id = `w${++this.seq}`;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new SelfvoyError('timeout', 'Сервер не ответил вовремя'));
      }, timeout);
      this.pending.set(id, { resolve: (p) => resolve(p as T), reject, timer });
      ws.send(JSON.stringify({ id, type, payload }));
    });
  }

  private handle(raw: string): void {
    let message: { id?: string; type?: string; event?: string; payload?: Payload; code?: string; message?: string };
    try {
      message = JSON.parse(raw);
    } catch {
      return;
    }
    if (message.type === 'event') {
      const event = { event: message.event ?? '', payload: message.payload ?? {} };
      this.eventSubs.forEach((fn) => fn(event));
      return;
    }
    const pending = message.id ? this.pending.get(String(message.id)) : undefined;
    if (!pending) return;
    this.pending.delete(String(message.id));
    clearTimeout(pending.timer);
    if (message.type === 'ok') pending.resolve(message.payload ?? {});
    else pending.reject(new SelfvoyError(message.code ?? 'error', message.message ?? 'Ошибка сервера'));
  }

  private waitReady(timeout: number): Promise<void> {
    if (this.ready) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const done = () => {
        clearTimeout(timer);
        resolve();
      };
      const timer = setTimeout(() => {
        this.waiters.delete(done);
        reject(new SelfvoyError('offline', 'Нет связи с сервером Selfvoy'));
      }, timeout);
      this.waiters.add(done);
    });
  }

  private dropPending(reason: string): void {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new SelfvoyError('disconnected', reason));
    }
    this.pending.clear();
  }

  private setState(state: ConnState): void {
    if (this.state === state) return;
    this.state = state;
    this.stateSubs.forEach((fn) => fn(state));
  }
}
