/*
  Виртуальный WebSocket для автономного режима (Virtual WebSocket).
  Реализует стандартный интерфейс W3C WebSocket и маршрутизирует сообщения
  в локальный StandaloneEngine без обращения к сетевому стеку.
*/

import { standaloneEngine } from './standaloneEngine';

export class VirtualWebSocket implements EventTarget {
  static readonly CONNECTING = 0;
  static readonly OPEN = 1;
  static readonly CLOSING = 2;
  static readonly CLOSED = 3;

  readonly CONNECTING = 0;
  readonly OPEN = 1;
  readonly CLOSING = 2;
  readonly CLOSED = 3;

  binaryType: BinaryType = 'blob';
  bufferedAmount = 0;
  extensions = '';
  protocol = '';
  readyState: number = VirtualWebSocket.CONNECTING;
  url: string;

  onclose: ((this: WebSocket, ev: CloseEvent) => any) | null = null;
  onerror: ((this: WebSocket, ev: Event) => any) | null = null;
  onmessage: ((this: WebSocket, ev: MessageEvent) => any) | null = null;
  onopen: ((this: WebSocket, ev: Event) => any) | null = null;

  private listeners: Map<string, Set<(ev: any) => void>> = new Map();

  constructor(url: string) {
    this.url = url;
    // Мгновенное открытие соединения
    setTimeout(() => {
      this.readyState = VirtualWebSocket.OPEN;
      const ev = new Event('open');
      if (this.onopen) {
        this.onopen.call(this as unknown as WebSocket, ev);
      }
      this.dispatch('open', ev);
    }, 15);
  }

  send(data: string | ArrayBufferLike | Blob | ArrayBufferView): void {
    if (this.readyState !== VirtualWebSocket.OPEN) {
      console.warn('[VirtualWebSocket] send called on closed socket');
      return;
    }
    if (typeof data !== 'string') return;

    try {
      const msg = JSON.parse(data);
      const response = standaloneEngine.handleRequest(msg, (eventMsg: any) => {
        if (this.readyState === VirtualWebSocket.OPEN) {
          const ev = new MessageEvent('message', { data: JSON.stringify(eventMsg) });
          if (this.onmessage) {
            this.onmessage.call(this as unknown as WebSocket, ev);
          }
          this.dispatch('message', ev);
        }
      });

      if (response) {
        setTimeout(() => {
          if (this.readyState === VirtualWebSocket.OPEN) {
            const ev = new MessageEvent('message', { data: JSON.stringify(response) });
            if (this.onmessage) {
              this.onmessage.call(this as unknown as WebSocket, ev);
            }
            this.dispatch('message', ev);
          }
        }, 10);
      }
    } catch (e) {
      console.error('[VirtualWebSocket] message error:', e);
    }
  }

  close(code = 1000, reason = 'Normal Closure'): void {
    if (this.readyState === VirtualWebSocket.CLOSED) return;
    this.readyState = VirtualWebSocket.CLOSED;
    const ev = new CloseEvent('close', { code, reason, wasClean: true });
    if (this.onclose) {
      this.onclose.call(this as unknown as WebSocket, ev);
    }
    this.dispatch('close', ev);
  }

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    let set = this.listeners.get(type);
    if (!set) {
      set = new Set();
      this.listeners.set(type, set);
    }
    set.add(typeof listener === 'function' ? listener : listener.handleEvent);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    const set = this.listeners.get(type);
    if (set) {
      set.delete(typeof listener === 'function' ? listener : listener.handleEvent);
    }
  }

  dispatchEvent(event: Event): boolean {
    this.dispatch(event.type, event);
    return true;
  }

  private dispatch(type: string, event: Event): void {
    const set = this.listeners.get(type);
    if (set) {
      set.forEach((fn) => {
        try {
          fn(event);
        } catch (e) {
          console.error(e);
        }
      });
    }
  }
}
