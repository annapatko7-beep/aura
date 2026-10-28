import { Capacitor } from '@capacitor/core';
import { PushNotifications, type ActionPerformed, type PushNotificationSchema, type Token } from '@capacitor/push-notifications';
import type { Api } from './api';

export interface PushStatus {
  supported: boolean;
  isNative: boolean;
  permission: 'granted' | 'denied' | 'prompt' | 'default';
  registered: boolean;
  token?: string;
  error?: string;
}

class PushService {
  private registeredToken: string | null = null;
  private onNotificationReceivedHandlers: Array<(notification: { title: string; body: string }) => void> = [];

  isSupported(): boolean {
    if (Capacitor.isNativePlatform()) return true;
    return typeof window !== 'undefined' && 'Notification' in window;
  }

  isNative(): boolean {
    return Capacitor.isNativePlatform();
  }

  async getPermissionStatus(): Promise<'granted' | 'denied' | 'prompt' | 'default'> {
    if (this.isNative()) {
      try {
        const status = await PushNotifications.checkPermissions();
        if (status.receive === 'granted') return 'granted';
        if (status.receive === 'denied') return 'denied';
        return 'prompt';
      } catch {
        return 'default';
      }
    }

    if (typeof window !== 'undefined' && 'Notification' in window) {
      return Notification.permission as 'granted' | 'denied' | 'default';
    }

    return 'denied';
  }

  async requestPermissionAndRegister(api?: Api): Promise<PushStatus> {
    if (!this.isSupported()) {
      return {
        supported: false,
        isNative: false,
        permission: 'denied',
        registered: false,
        error: 'Push-уведомления не поддерживаются на этом устройстве'
      };
    }

    // 1. Native platform (Android / iOS via Capacitor)
    if (this.isNative()) {
      try {
        let permStatus = await PushNotifications.checkPermissions();
        if (permStatus.receive !== 'granted') {
          permStatus = await PushNotifications.requestPermissions();
        }

        if (permStatus.receive !== 'granted') {
          return {
            supported: true,
            isNative: true,
            permission: 'denied',
            registered: false,
            error: 'Разрешение на push-уведомления отклонено в настройках системы'
          };
        }

        // Setup listeners if not already done
        await this.setupNativeListeners(api);

        // Register with Apple / Google FCM
        await PushNotifications.register();

        return {
          supported: true,
          isNative: true,
          permission: 'granted',
          registered: true,
          token: this.registeredToken || undefined
        };
      } catch (e) {
        const errorMsg = e instanceof Error ? e.message : String(e);
        return {
          supported: true,
          isNative: true,
          permission: 'denied',
          registered: false,
          error: errorMsg
        };
      }
    }

    // 2. Web browser (PWA / Desktop / Mobile Browser)
    if (typeof window !== 'undefined' && 'Notification' in window) {
      try {
        const permission = await Notification.requestPermission();
        if (permission === 'granted') {
          // Send simulated/browser registration to server
          const pseudoToken = 'web_' + (navigator.userAgent.replace(/[^a-zA-Z0-9]/g, '').slice(0, 32)) + '_' + Date.now();
          this.registeredToken = pseudoToken;
          if (api) {
            try {
              await api.registerPushDevice('dev', pseudoToken);
            } catch {
              // Silently ignore if server offline
            }
          }

          return {
            supported: true,
            isNative: false,
            permission: 'granted',
            registered: true,
            token: pseudoToken
          };
        }

        return {
          supported: true,
          isNative: false,
          permission: permission as 'denied' | 'default',
          registered: false,
          error: 'Разрешение на уведомления в браузере не предоставлено'
        };
      } catch (e) {
        return {
          supported: true,
          isNative: false,
          permission: 'denied',
          registered: false,
          error: e instanceof Error ? e.message : String(e)
        };
      }
    }

    return {
      supported: false,
      isNative: false,
      permission: 'denied',
      registered: false
    };
  }

  async showLocalNotification(title: string, body: string, linkUrl?: string): Promise<void> {
    if (typeof window === 'undefined') return;
    if (!('Notification' in window) || Notification.permission !== 'granted') return;

    // 1. Android Chrome / Mobile PWA requires ServiceWorkerRegistration.showNotification
    try {
      if ('serviceWorker' in navigator) {
        const reg = await navigator.serviceWorker.getRegistration();
        if (reg && reg.showNotification) {
          await reg.showNotification(title, {
            body,
            icon: '/favicon.ico',
            badge: '/favicon.ico',
            data: { url: linkUrl || window.location.href }
          });
          return;
        }
      }
    } catch {
      // Fallback to Notification constructor
    }

    // 2. Desktop Chrome / Safari / Firefox
    try {
      const n = new Notification(title, {
        body,
        icon: '/favicon.ico',
        badge: '/favicon.ico'
      });
      n.onclick = () => {
        window.focus();
        if (linkUrl) {
          window.location.href = linkUrl;
        }
        n.close();
      };
    } catch {
      // Fallback for environments without notification constructor
    }
  }

  onNotificationReceived(handler: (n: { title: string; body: string }) => void) {
    this.onNotificationReceivedHandlers.push(handler);
    return () => {
      this.onNotificationReceivedHandlers = this.onNotificationReceivedHandlers.filter(h => h !== handler);
    };
  }

  private async setupNativeListeners(api?: Api) {
    await PushNotifications.removeAllListeners();

    await PushNotifications.addListener('registration', async (token: Token) => {
      this.registeredToken = token.value;
      if (api) {
        try {
          await api.registerPushDevice('fcm', token.value);
        } catch (e) {
          console.warn('[Push] Failed to register device token on server:', e);
        }
      }
    });

    await PushNotifications.addListener('registrationError', (err) => {
      console.error('[Push] Registration error:', err);
    });

    await PushNotifications.addListener('pushNotificationReceived', (notification: PushNotificationSchema) => {
      this.onNotificationReceivedHandlers.forEach(h => h({
        title: notification.title || 'Новое уведомление',
        body: notification.body || ''
      }));
    });

    await PushNotifications.addListener('pushNotificationActionPerformed', (action: ActionPerformed) => {
      const data = action.notification.data;
      if (data && data.linkUrl) {
        window.location.href = data.linkUrl;
      }
    });
  }
}

export const pushService = new PushService();
