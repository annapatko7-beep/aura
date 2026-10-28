import { Capacitor } from '@capacitor/core';

export type Platform = 'ios' | 'android' | 'desktop' | 'web';

interface DesktopBridge {
  platform: 'darwin' | 'win32' | 'linux';
}

declare global {
  interface Window {
    selfvoyDesktop?: DesktopBridge;
  }
}

export const isNative = Capacitor.isNativePlatform();

export function getPlatform(): Platform {
  const p = Capacitor.getPlatform();
  if (p === 'ios' || p === 'android') return p;
  if (window.selfvoyDesktop) return 'desktop';
  return 'web';
}

/** Operating system of the desktop shell, so CSS can leave room for macOS window buttons. */
export function desktopOs(): DesktopBridge['platform'] | null {
  return window.selfvoyDesktop?.platform ?? null;
}
