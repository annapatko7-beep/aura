import type { CapacitorConfig } from '@capacitor/cli';

const config: CapacitorConfig = {
  appId: 'app.selfvoy.network',
  appName: 'Selfvoy',
  webDir: 'dist',
  backgroundColor: '#0b0c10',
  ios: {
    contentInset: 'never',
    backgroundColor: '#0b0c10',
  },
  android: {
    backgroundColor: '#0b0c10',
    // Страница приложения открыта как https://localhost, а сервер в локальной сети —
    // ws://192.168.x.x:9000. Без этого WebView блокирует такое соединение как
    // «смешанное содержимое». Продакшн-адрес — wss://, ему это не нужно.
    allowMixedContent: true,
  },
  plugins: {
    SplashScreen: {
      launchAutoHide: false,
      backgroundColor: '#0b0c10',
      showSpinner: false,
    },
    Keyboard: {
      resize: 'body',
      style: 'DARK',
    },
    StatusBar: {
      style: 'DARK',
      overlaysWebView: true,
    },
  },
};

export default config;
