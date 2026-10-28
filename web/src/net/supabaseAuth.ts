/*
  Интеграция с Supabase Auth:
  Реальная облачная регистрация, вход по email/паролю и валидные сессионные токены.
*/

export interface SupabaseUser {
  id: string;
  email: string;
  user_metadata?: {
    display_name?: string;
  };
  created_at: string;
  email_confirmed_at?: string;
}

export interface SupabaseAuthResponse {
  access_token: string;
  refresh_token: string;
  token_type: string;
  expires_in: number;
  user: SupabaseUser;
}

export function supabaseUserToSelfvoy(sbUser: SupabaseUser) {
  let numId = 1;
  if (sbUser.id) {
    let h = 0;
    for (let i = 0; i < sbUser.id.length; i++) {
      h = (h << 5) - h + sbUser.id.charCodeAt(i);
      h |= 0;
    }
    numId = Math.abs(h) || 1;
  }
  return {
    id: numId,
    email: sbUser.email,
    display_name: sbUser.user_metadata?.display_name || sbUser.email.split('@')[0],
    avatar_url: '',
    bio: '',
    email_verified: Boolean(sbUser.email_confirmed_at),
    role: 'user' as const,
  };
}

const SUPABASE_URL = (import.meta.env.VITE_SUPABASE_URL || 'https://rrlhxttjebsommbwusij.supabase.co').replace(/\/$/, '');
const SUPABASE_ANON_KEY =
  import.meta.env.VITE_SUPABASE_ANON_KEY ||
  'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InJybGh4dHRqZWJzb21tYnd1c2lqIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTI4MzA1NTcsImV4cCI6MjEwODQwNjU1N30.dWEjANTcmXXdrdgKOMFPEa9paAHoo-XCKuuvC6ZwAi0';

export function isSupabaseAvailable(): boolean {
  return Boolean(SUPABASE_URL && SUPABASE_ANON_KEY);
}

export async function supabaseSignUp(email: string, password: string, displayName: string): Promise<SupabaseAuthResponse> {
  const resp = await fetch(`${SUPABASE_URL}/auth/v1/signup`, {
    method: 'POST',
    headers: {
      apikey: SUPABASE_ANON_KEY,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      email: email.trim().toLowerCase(),
      password,
      data: { display_name: displayName.trim() },
    }),
  });

  const data = await resp.json();
  if (!resp.ok) {
    throw new Error(data.msg || data.error_description || data.message || 'Ошибка регистрации в Supabase');
  }

  // Если сессия сразу выдана (autoconfirm включен в проекте)
  if (data.access_token) {
    return data as SupabaseAuthResponse;
  }

  // Если сессия во вложенном объекте
  if (data.session?.access_token) {
    return {
      access_token: data.session.access_token,
      refresh_token: data.session.refresh_token,
      token_type: data.session.token_type,
      expires_in: data.session.expires_in,
      user: data.user,
    };
  }

  // Автоматический вход после успешной регистрации
  return supabaseSignIn(email, password);
}

export async function supabaseSignIn(email: string, password: string): Promise<SupabaseAuthResponse> {
  const resp = await fetch(`${SUPABASE_URL}/auth/v1/token?grant_type=password`, {
    method: 'POST',
    headers: {
      apikey: SUPABASE_ANON_KEY,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({
      email: email.trim().toLowerCase(),
      password,
    }),
  });

  const data = await resp.json();
  if (!resp.ok) {
    throw new Error(data.msg || data.error_description || data.message || 'Неверный email или пароль');
  }

  return data as SupabaseAuthResponse;
}

export function supabaseOAuthUrl(provider: string, redirectTo: string): string {
  return `${SUPABASE_URL}/auth/v1/authorize?provider=${provider}&redirect_to=${encodeURIComponent(redirectTo)}`;
}

