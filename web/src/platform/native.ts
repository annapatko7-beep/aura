import { App } from '@capacitor/app';
import { Haptics, ImpactStyle, NotificationType } from '@capacitor/haptics';
import { Keyboard } from '@capacitor/keyboard';
import { SplashScreen } from '@capacitor/splash-screen';
import { StatusBar, Style } from '@capacitor/status-bar';
import { desktopOs, getPlatform, isNative } from './index';

/**
 * One-time setup for the shell the app runs in. Marks <html> with the platform so CSS can adapt
 * (safe areas on phones, window-drag regions on desktop) and wires the native status bar,
 * keyboard and splash screen.
 */
export async function initPlatform(): Promise<void> {
  const platform = getPlatform();
  const root = document.documentElement;
  root.dataset.platform = platform;
  const os = desktopOs();
  if (os) root.dataset.os = os;

  if (!isNative) return;
  try {
    // Цвет текста строки состояния — под тему системы (светлая/тёмная).
    const dark = typeof matchMedia === 'function' && matchMedia('(prefers-color-scheme: dark)').matches;
    await StatusBar.setStyle({ style: dark ? Style.Dark : Style.Light });
    if (platform === 'android') {
      await StatusBar.setOverlaysWebView({ overlay: true });
      await StatusBar.setBackgroundColor({ color: '#00000000' });
    }
    await Keyboard.setAccessoryBarVisible({ isVisible: false }).catch(() => undefined);
  } finally {
    await SplashScreen.hide({ fadeOutDuration: 250 });
  }
}

/**
 * Android hardware back: go back in history, or leave the app from a root screen.
 * Returns an unsubscribe function.
 */
export function onBackButton(goBack: () => void, isRoot: () => boolean): () => void {
  if (getPlatform() !== 'android') return () => undefined;
  const handle = App.addListener('backButton', () => {
    if (isRoot()) App.minimizeApp();
    else goBack();
  });
  return () => {
    handle.then((h) => h.remove());
  };
}

/** Light tap feedback on phones; a no-op on desktop and web. */
export function tap(kind: 'light' | 'success' = 'light'): void {
  if (!isNative) return;
  if (kind === 'success') Haptics.notification({ type: NotificationType.Success }).catch(() => undefined);
  else Haptics.impact({ style: ImpactStyle.Light }).catch(() => undefined);
}
