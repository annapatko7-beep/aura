import { defineConfig, type Plugin } from 'vite';
import react from '@vitejs/plugin-react';

/*
  Content-Security-Policy для собранного приложения (Electron, Capacitor, браузер).
  Код и шрифты — только свои; соединения — к серверу Selfvoy по ws/wss (адрес задаёт
  пользователь, поэтому хост заранее неизвестен); картинки — свои, data: (QR-код,
  аватар) и https: (аватар по ссылке). В dev-режиме политика не ставится: Vite
  подмешивает свои inline-скрипты для горячей перезагрузки.
*/
export const CSP = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self' 'unsafe-inline'",
  "font-src 'self' data:",
  "img-src 'self' data: blob: https:",
  "media-src 'self' blob:",
  'connect-src ws: wss: http: https:',
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'none'",
].join('; ');

function contentSecurityPolicy(): Plugin {
  return {
    name: 'selfvoy-csp',
    apply: 'build',
    transformIndexHtml: () => [{ tag: 'meta', attrs: { 'http-equiv': 'Content-Security-Policy', content: CSP }, injectTo: 'head-prepend' }],
  };
}

export default defineConfig({
  plugins: [react(), contentSecurityPolicy()],
  base: './',
});
