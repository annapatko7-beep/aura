import { useEffect, useState } from 'react';
import { Spinner } from './ui';
import { useSession, errorText } from '../state/session';
import type { OAuthProvider } from '../net/api';
import { isSupabaseAvailable, supabaseOAuthUrl } from '../net/supabaseAuth';
import { saveTokens } from '../net/secureStore';
import { cacheUser } from '../net/userCache';

const DEFAULT_PROVIDERS: OAuthProvider[] = [
  { provider: 'google', name: 'Google', icon: 'google', enabled: true },
  { provider: 'yandex', name: 'Яндекс ID', icon: 'yandex', enabled: true },
  { provider: 'github', name: 'GitHub', icon: 'github', enabled: true },
  { provider: 'vk', name: 'VK ID', icon: 'vk', enabled: true },
  { provider: 'discord', name: 'Discord', icon: 'discord', enabled: true },
];

function GoogleIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 24 24">
      <path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92c-.26 1.37-1.04 2.53-2.21 3.31v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.09z" />
      <path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z" />
      <path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.06H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.94l2.85-2.22.81-.63z" />
      <path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.06l3.66 2.84c.87-2.6 3.3-4.52 6.16-4.52z" />
    </svg>
  );
}

function YandexIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 24 24">
      <circle cx="12" cy="12" r="11" fill="#FC3F1D" />
      <path fill="#FFFFFF" d="M13.8 6.5h-1.6c-2.1 0-3.3 1.1-3.3 2.8 0 1.5.8 2.4 2.1 3.3l-2.6 4.9h2l2.9-5.4h.1v5.4h1.8V6.5zm-1.8 4.2c-.8-.6-1.3-1.1-1.3-1.9 0-.9.6-1.5 1.5-1.5h.3v3.4h-.5z" />
    </svg>
  );
}

function GithubIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">
      <path fillRule="evenodd" clipRule="evenodd" d="M12 2C6.477 2 2 6.484 2 12.017c0 4.425 2.865 8.18 6.839 9.504.5.092.682-.217.682-.483 0-.237-.008-.868-.013-1.703-2.782.605-3.369-1.343-3.369-1.343-.454-1.158-1.11-1.466-1.11-1.466-.908-.62.069-.608.069-.608 1.003.07 1.53 1.032 1.53 1.032.892 1.53 2.341 1.088 2.91.832.092-.647.35-1.088.636-1.338-2.22-.253-4.555-1.113-4.555-4.951 0-1.093.39-1.988 1.029-2.688-.103-.253-.446-1.272.098-2.65 0 0 .84-.27 2.75 1.026A9.564 9.564 0 0112 6.844c.85.004 1.705.115 2.504.337 1.909-1.296 2.747-1.027 2.747-1.027.546 1.379.202 2.398.1 2.651.64.7 1.028 1.595 1.028 2.688 0 3.848-2.339 4.695-4.566 4.943.359.309.678.92.678 1.855 0 1.338-.012 2.419-.012 2.747 0 .268.18.58.688.482A10.019 10.019 0 0022 12.017C22 6.484 17.522 2 12 2z" />
    </svg>
  );
}

function VkIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="#0077FF">
      <path d="M13.162 18.994c.609 0 .858-.406.851-.915-.072-1.428.64-1.942 1.341-1.341.928.796 1.391 1.763 2.464 2.256h2.247c.507 0 .761-.253.616-.761-.318-1.114-1.439-2.217-2.327-3.086-.745-.731-.564-1.056 0-1.808 1.405-1.876 3.09-4.144 2.516-4.908-.346-.462-1.113-.428-1.748-.428h-2.15c-.443 0-.671.189-.838.601-.84 2.072-2.138 4.22-2.825 4.397-.333.086-.484-.044-.484-.521V9.324c0-.756-.221-.926-.649-.926h-3.41c-.428 0-.649.317-.649.616 0 .614.919.756 1.014 2.483v3.748c0 .822-.148.974-.476.974-.875 0-3.003-3.136-4.267-6.721-.248-.703-.49-.998-1.196-.998H1.365c-.633 0-.761.298-.761.633 0 .591.758 3.542 3.535 7.426 1.854 2.614 4.462 4.027 6.837 4.027l2.186-.59z" />
    </svg>
  );
}

function DiscordIcon() {
  return (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="#5865F2">
      <path d="M20.317 4.37a19.791 19.791 0 0 0-4.885-1.515.074.074 0 0 0-.079.037c-.21.375-.444.864-.608 1.25a18.27 18.27 0 0 0-5.487 0 12.64 12.64 0 0 0-.617-1.25.077.077 0 0 0-.079-.037A19.736 19.736 0 0 0 3.677 4.37a.07.07 0 0 0-.032.027C.533 9.046-.32 13.58.099 18.057a.082.082 0 0 0 .031.057 19.9 19.9 0 0 0 5.993 3.03.078.078 0 0 0 .084-.028c.462-.63.874-1.295 1.226-1.994.021-.041.001-.09-.041-.106a13.107 13.107 0 0 1-1.872-.892.077.077 0 0 1-.008-.128 10.2 10.2 0 0 0 .372-.292.074.074 0 0 1 .077-.01c3.929 1.793 8.18 1.793 12.061 0a.074.074 0 0 1 .078.01c.12.098.246.198.373.292a.077.077 0 0 1-.006.127 12.299 12.299 0 0 1-1.873.894.077.077 0 0 0-.041.107c.36.698.772 1.362 1.225 1.993a.076.076 0 0 0 .084.028 19.839 19.839 0 0 0 6.002-3.03.077.077 0 0 0 .032-.054c.5-5.177-.838-9.674-3.549-13.66a.061.061 0 0 0-.031-.028zM8.02 15.33c-1.183 0-2.157-1.085-2.157-2.419 0-1.333.956-2.419 2.157-2.419 1.21 0 2.176 1.096 2.157 2.42 0 1.333-.956 2.418-2.157 2.418zm7.975 0c-1.183 0-2.157-1.085-2.157-2.419 0-1.333.955-2.419 2.157-2.419 1.21 0 2.176 1.096 2.157 2.42 0 1.333-.946 2.418-2.157 2.418z" />
    </svg>
  );
}

function renderProviderIcon(provider: string) {
  switch (provider) {
    case 'google':
      return <GoogleIcon />;
    case 'yandex':
      return <YandexIcon />;
    case 'github':
      return <GithubIcon />;
    case 'vk':
      return <VkIcon />;
    case 'discord':
      return <DiscordIcon />;
    default:
      return null;
  }
}

export function OAuthButtons({ onSuccess, onError }: { onSuccess: () => void; onError?: (err: string) => void }) {
  const { api, loginOAuth, conn } = useSession();
  const [providers, setProviders] = useState<OAuthProvider[]>(DEFAULT_PROVIDERS);
  const [busyProvider, setBusyProvider] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    if (conn === 'open') {
      api
        .oauthProviders()
        .then((res) => {
          if (active && res.providers && res.providers.length > 0) {
            setProviders(res.providers);
          }
        })
        .catch(() => {
          /* используем дефолтные */
        });
    }
    return () => {
      active = false;
    };
  }, [api, conn]);

  useEffect(() => {
    const handleMessage = (e: MessageEvent) => {
      if (e.data && e.data.type === 'selfvoy:oauth') {
        const { provider, code, state } = e.data;
        if (provider && code && state) {
          setBusyProvider(provider);
          loginOAuth(provider, code, state)
            .then(() => onSuccess())
            .catch((err) => {
              if (onError) onError(errorText(err));
            })
            .finally(() => setBusyProvider(null));
        }
      } else if (e.data && e.data.type === 'selfvoy:supabase_oauth') {
        const { token, refresh_token, user } = e.data;
        if (token && user) {
          saveTokens({ access: token, refresh: refresh_token || '' });
          cacheUser(user);
          onSuccess();
        }
      }
    };
    window.addEventListener('message', handleMessage);
    return () => window.removeEventListener('message', handleMessage);
  }, [loginOAuth, onSuccess, onError]);

  const handleOAuthClick = async (provider: string) => {
    if (busyProvider) return;
    setBusyProvider(provider);
    try {
      const redirectUri =
        window.location.protocol === 'file:'
          ? 'http://127.0.0.1:9000/auth/oauth/callback'
          : `${window.location.origin}/oauth/callback`;

      let authorizeUrl = '';
      if (conn === 'open') {
        const result = await api.oauthBegin(provider, redirectUri);

        // В dev mock режиме, если URL содержит mock_code
        if (result.authorize_url.includes('mock_code')) {
          await loginOAuth(provider, 'mock_code_123', result.state);
          onSuccess();
          return;
        }
        authorizeUrl = result.authorize_url;
      } else if (isSupabaseAvailable() && provider === 'google') {
        authorizeUrl = supabaseOAuthUrl('google', redirectUri);
      } else {
        throw new Error('Нет связи с сервером Selfvoy');
      }

      // Открытие окна авторизации
      const width = 560;
      const height = 680;
      const left = window.screenX + (window.outerWidth - width) / 2;
      const top = window.screenY + (window.outerHeight - height) / 2;
      const popup = window.open(
        authorizeUrl,
        `selfvoy_oauth_${provider}`,
        `width=${width},height=${height},left=${left},top=${top},status=0,toolbar=0,menubar=0`,
      );

      if (!popup || popup.closed) {
        // Если попап заблокирован — открываем в текущей вкладке
        window.location.href = authorizeUrl;
      }
    } catch (err) {
      if (onError) onError(errorText(err));
    } finally {
      setBusyProvider(null);
    }
  };

  return (
    <div className="stack" style={{ gap: 10, marginTop: 4 }}>
      <div className="row" style={{ alignItems: 'center', gap: 12, margin: '8px 0 2px' }}>
        <div style={{ flex: 1, height: 1, background: 'var(--border, rgba(255,255,255,0.12))' }} />
        <span style={{ fontSize: 13, color: 'var(--muted, #8e8e93)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
          или быстрый вход
        </span>
        <div style={{ flex: 1, height: 1, background: 'var(--border, rgba(255,255,255,0.12))' }} />
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: '1fr', gap: 8 }}>
        {providers.map((p) => {
          const isBusy = busyProvider === p.provider;
          return (
            <button
              key={p.provider}
              type="button"
              className="btn btn-glass"
              style={{
                width: '100%',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                gap: 10,
                padding: '10px 16px',
                borderRadius: 'var(--radius, 12px)',
                fontWeight: 500,
                fontSize: 14,
              }}
              disabled={Boolean(busyProvider) || !p.enabled}
              onClick={() => handleOAuthClick(p.provider)}
            >
              {isBusy ? <Spinner small /> : renderProviderIcon(p.provider)}
              <span>Продолжить с {p.name}</span>
            </button>
          );
        })}
      </div>
    </div>
  );
}
