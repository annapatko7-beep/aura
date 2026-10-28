/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Основной адрес сервера Selfvoy (ws:// или wss://), задаётся при сборке. */
  readonly VITE_SELFVOY_SERVER_URL?: string;
  /** Запасной адрес: клиент переключается на него, если основной не отвечает. */
  readonly VITE_SELFVOY_FALLBACK_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
