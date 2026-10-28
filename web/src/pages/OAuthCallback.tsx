import { useEffect, useState } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { useSession, errorText } from '../state/session';
import { Logo, Spinner } from '../components/ui';
import { supabaseUserToSelfvoy } from '../net/supabaseAuth';
import { saveTokens } from '../net/secureStore';
import { cacheUser } from '../net/userCache';

export function OAuthCallback() {
  const [searchParams] = useSearchParams();
  const navigate = useNavigate();
  const { loginOAuth } = useSession();
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    // 1. Проверяем токен от Supabase OAuth (#access_token=...&refresh_token=...)
    const rawHash = window.location.hash.startsWith('#') ? window.location.hash.slice(1) : window.location.hash;
    const hashParams = new URLSearchParams(rawHash.includes('?') ? rawHash.split('?')[1] : rawHash);
    const urlParams = new URLSearchParams(window.location.search);
    const accessToken = hashParams.get('access_token') || searchParams.get('access_token') || urlParams.get('access_token');
    const refreshToken = hashParams.get('refresh_token') || searchParams.get('refresh_token') || urlParams.get('refresh_token');

    if (accessToken) {
      const supUrl = (import.meta.env.VITE_SUPABASE_URL || 'https://rrlhxttjebsommbwusij.supabase.co').replace(/\/$/, '');
      const supKey =
        import.meta.env.VITE_SUPABASE_ANON_KEY ||
        'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InJybGh4dHRqZWJzb21tYnd1c2lqIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTI4MzA1NTcsImV4cCI6MjEwODQwNjU1N30.dWEjANTcmXXdrdgKOMFPEa9paAHoo-XCKuuvC6ZwAi0';

      fetch(`${supUrl}/auth/v1/user`, {
        headers: { apikey: supKey, Authorization: `Bearer ${accessToken}` },
      })
        .then((r) => r.json())
        .then((user) => {
          const selfvoyUser = supabaseUserToSelfvoy(user);
          if (window.opener && window.opener !== window) {
            window.opener.postMessage(
              {
                type: 'selfvoy:supabase_oauth',
                token: accessToken,
                refresh_token: refreshToken || '',
                user: selfvoyUser,
              },
              '*',
            );
            setTimeout(() => window.close(), 600);
          } else {
            saveTokens({ access: accessToken, refresh: refreshToken || '' });
            cacheUser(selfvoyUser);
            window.location.href = '/#/';
          }
        })
        .catch((e) => setError(String(e)));
      return;
    }

    // 2. Параметры авторизации C++ сервера (?code=...&state=...)
    const code = searchParams.get('code') || hashParams.get('code') || urlParams.get('code') || '';
    const state = searchParams.get('state') || hashParams.get('state') || urlParams.get('state') || '';
    const provider = searchParams.get('provider') || hashParams.get('provider') || urlParams.get('provider') || 'google';

    if (!code || !state) {
      setError('Не найдены параметры авторизации (code или state).');
      return;
    }

    // Если страница открыта во всплывающем окне (popup)
    if (window.opener && window.opener !== window) {
      try {
        window.opener.postMessage(
          {
            type: 'selfvoy:oauth',
            provider,
            code,
            state,
          },
          '*',
        );
        setTimeout(() => window.close(), 600);
        return;
      } catch {
        /* fallback to direct login */
      }
    }

    // Прямой вход в текущей вкладке
    loginOAuth(provider, code, state)
      .then(() => {
        navigate('/', { replace: true });
      })
      .catch((err) => {
        setError(errorText(err));
      });
  }, [searchParams, loginOAuth, navigate]);

  return (
    <div className="auth">
      <div className="auth-card" style={{ textAlign: 'center' }}>
        <span className="logo">
          <Logo />
        </span>
        <h1 className="title-lg" style={{ marginTop: 12 }}>
          Вход в Selfvoy
        </h1>
        {error ? (
          <div className="stack" style={{ gap: 14, marginTop: 16 }}>
            <div className="notice error">{error}</div>
            <button type="button" className="btn btn-primary" onClick={() => navigate('/login')}>
              Вернуться ко входу
            </button>
          </div>
        ) : (
          <div className="stack" style={{ alignItems: 'center', gap: 14, margin: '24px 0' }}>
            <Spinner />
            <p className="lead" style={{ margin: 0 }}>
              Завершаем вход в аккаунт...
            </p>
          </div>
        )}
      </div>
    </div>
  );
}
