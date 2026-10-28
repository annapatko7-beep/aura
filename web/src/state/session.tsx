/*
  Сессия: соединение с сервером + аккаунт.

  Жизненный цикл:
  1. старт — читаем токены; если есть, сразу показываем приложение с
     последним известным профилем (офлайн тоже работает), иначе — вход;
  2. при каждом открытии сокета (onOpen) повторно авторизуемся: auth.token,
     при отказе — auth.refresh под межвкладочной блокировкой (state/reauth.ts),
     при отказе и его — выходим на экран входа; сетевая ошибка — переподключение;
  3. вход: auth.login → при включённой 2FA сервер отвечает requires_2fa —
     спрашиваем код и шлём auth.login2fa; при неподтверждённом email —
     экран кода из письма (auth.verifyEmail).

  Выход и смена адреса сервера сначала удаляют токены и кэш пользователя с
  устройства (не ждут сервер), а уже потом — best-effort auth.logout. Токены
  одного сервера никогда не уходят на другой.

  Пароль держится только в памяти на время многошагового входа (2FA, код из
  письма) и нигде не сохраняется.
*/
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { currentEnv, loadOverride, normalizeServerUrl, sameServers, saveOverride, serverCandidates } from '../net/config';
import { SelfvoyError, SelfvoySocket, type ConnState } from '../net/socket';
import { makeApi, type Api, type AuthPayload, type User } from '../net/api';
import { clearTokens, deviceInfo, loadTokens, saveTokens } from '../net/secureStore';
import { cachedUser, cacheUser, clearUserCaches } from '../net/userCache';
import { getPlatform } from '../platform';
import { reauthenticate, withRefreshLock } from './reauth';
import { isSupabaseAvailable, supabaseSignIn, supabaseSignUp, supabaseUserToSelfvoy } from '../net/supabaseAuth';

export type AuthStatus = 'booting' | 'signedOut' | 'signedIn';

/** Итог шага входа: дальше приложение, ввод 2FA-кода или кода из письма. */
export type LoginStep = { kind: 'done' } | { kind: 'twoFactor' } | { kind: 'verifyEmail'; devCode?: string };

interface Session {
  api: Api;
  socket: SelfvoySocket;
  conn: ConnState;
  /** Было ли хоть одно успешное подключение за этот запуск. */
  everConnected: boolean;
  serverUrl: string;
  connError: string;
  status: AuthStatus;
  user: User | null;
  /** Email, ожидающий кода (регистрация / вход до подтверждения). */
  pendingEmail: string;

  login(email: string, password: string): Promise<LoginStep>;
  loginOAuth(provider: string, code: string, state: string): Promise<void>;
  login2fa(code: string, trustDevice: boolean): Promise<void>;
  register(name: string, email: string, password: string, ref?: { code: string; source: string } | null): Promise<{ devCode?: string; done?: boolean }>;
  verifyEmail(code: string): Promise<LoginStep>;
  resendCode(): Promise<{ devCode?: string }>;
  forgotPassword(email: string): Promise<{ devCode?: string }>;
  resetPassword(email: string, code: string, newPassword: string): Promise<LoginStep>;
  /** Смена пароля; сервер выдаёт этому устройству новую пару токенов (старые отозваны). */
  changePassword(oldPassword: string, newPassword: string): Promise<void>;
  logout(): Promise<void>;
  setUser(user: User): void;
  setServer(input: string | null): string | null;
  retry(): void;
}

const Ctx = createContext<Session | null>(null);
/** Сколько ждать ответа на auth.logout: токены к этому моменту уже удалены с устройства. */
const LOGOUT_TIMEOUT_MS = 5000;

function deviceName(): string {
  const platform = getPlatform();
  if (platform === 'android') return 'Selfvoy для Android';
  if (platform === 'ios') return 'Selfvoy для iPhone';
  if (platform === 'desktop') return 'Selfvoy Desktop';
  return 'Selfvoy в браузере';
}

export function SessionProvider({ children }: { children: ReactNode }) {
  const [conn, setConn] = useState<ConnState>('connecting');
  const [everConnected, setEverConnected] = useState(false);
  const [status, setStatus] = useState<AuthStatus>('booting');
  const [user, setUserState] = useState<User | null>(null);
  const [pendingEmail, setPendingEmail] = useState('');
  const [serverUrl, setServerUrl] = useState('');
  const [connError, setConnError] = useState('');
  // Адрес, заданный пользователем; читается сокетом при каждой попытке подключения.
  const overrideRef = useRef<string | null>(loadOverride());
  // Учётные данные многошагового входа — только в памяти.
  const pending = useRef<{ email: string; password: string } | null>(null);
  // «Эпоха» входа: растёт при входе, выходе и смене сервера. Авторизация на
  // соединении, начатая в прошлой эпохе, свой результат не применяет (иначе
  // токены, обновлённые для старого сервера, вернулись бы после выхода).
  const epoch = useRef(0);

  const applyUser = useCallback((next: User | null) => {
    setUserState(next);
    cacheUser(next);
  }, []);

  const signedIn = useCallback(
    async (payload: AuthPayload) => {
      if (!payload.token || !payload.refresh_token) throw new SelfvoyError('bad_response', 'Сервер не выдал токены входа');
      epoch.current += 1;
      await saveTokens({ access: payload.token, refresh: payload.refresh_token });
      applyUser(payload.user);
      pending.current = null;
      setPendingEmail('');
      setStatus('signedIn');
    },
    [applyUser],
  );

  /** Выход на этом устройстве: экран входа сразу, токены и (по желанию) кэш пользователя — долой. */
  const signOutLocal = useCallback(async (dropCaches: boolean) => {
    epoch.current += 1;
    pending.current = null;
    setPendingEmail('');
    setUserState(null);
    setStatus('signedOut');
    if (dropCaches) clearUserCaches();
    else cacheUser(null);
    await clearTokens().catch(() => undefined);
  }, []);

  // Один сокет на всё время жизни приложения.
  const socket = useMemo(
    () =>
      new SelfvoySocket({
        candidates: () => {
          const list = serverCandidates(overrideRef.current, currentEnv());
          if (!overrideRef.current && !list.includes('selfvoy://standalone')) {
            list.push('selfvoy://standalone');
          }
          return list;
        },
        fallbackToStandalone: true,
        onOpen: async (sock) => {
          const started = epoch.current;
          const result = await reauthenticate({
            request: (type, payload) => sock.requestNow(type, payload),
            load: loadTokens,
            save: saveTokens,
            clear: clearTokens,
            lock: withRefreshLock,
            device: deviceName(),
            stale: () => epoch.current !== started,
          });
          // Сетевая ошибка пробрасывается из reauthenticate: сокет переподключится.
          if (result.kind === 'stale') return;
          if (result.kind === 'signedIn') {
            applyUser(result.user);
            setStatus('signedIn');
            return;
          }
          // Токенов нет (или их отозвали) — экран входа, а не «вошёл без токенов».
          // Незавершённый вход (2FA, код из письма) при этом не сбрасываем.
          applyUser(null);
          setStatus('signedOut');
        },
      }),
    [applyUser],
  );
  const api = useMemo(() => makeApi(socket), [socket]);

  useEffect(() => {
    // Старт: с токенами сразу открываем приложение (последний профиль из кэша),
    // сеть подтянет актуальное; без токенов — экран входа.
    // Если сокет успел авторизоваться раньше, его результат не перетираем.
    loadTokens().then((tokens) => {
      const cached = cachedUser();
      if (tokens && cached) {
        setUserState((u) => u ?? cached);
        setStatus((s) => (s === 'booting' ? 'signedIn' : s));
      } else if (!tokens) {
        setStatus((s) => (s === 'booting' ? 'signedOut' : s));
      }
    });
    const offState = socket.onState((state) => {
      setConn(state);
      setServerUrl(socket.url);
      setConnError(socket.lastError);
      if (state === 'open') setEverConnected(true);
    });
    socket.start();
    return () => {
      offState();
      socket.stop();
    };
  }, [socket]);

  const login = useCallback(
    async (email: string, password: string): Promise<LoginStep> => {
      const address = email.trim().toLowerCase();
      try {
        const device = await deviceInfo();
        const payload = await api.login(address, password, deviceName(), device.id);
        await signedIn(payload);
        return { kind: 'done' };
      } catch (error) {
        if (error instanceof SelfvoyError && error.code === 'requires_2fa') {
          pending.current = { email: address, password };
          return { kind: 'twoFactor' };
        }
        if (error instanceof SelfvoyError && error.code === 'email_not_verified') {
          pending.current = { email: address, password };
          setPendingEmail(address);
          const sent = await api.resendCode(address).catch(() => ({ email_code: undefined }));
          return { kind: 'verifyEmail', devCode: sent.email_code };
        }
        // Если основной сервер не ответил, пробуем вход через Supabase Auth
        if (isSupabaseAvailable()) {
          try {
            const sbAuth = await supabaseSignIn(address, password);
            await signedIn({
              token: sbAuth.access_token,
              refresh_token: sbAuth.refresh_token,
              user: supabaseUserToSelfvoy(sbAuth.user),
            });
            return { kind: 'done' };
          } catch (sbErr: any) {
            if (error instanceof SelfvoyError && error.code !== 'socket_closed' && error.code !== 'timeout') {
              throw error;
            }
            throw new SelfvoyError('unauthorized', sbErr.message || 'Неверный email или пароль');
          }
        }
        throw error;
      }
    },
    [api, signedIn],
  );

  const login2fa = useCallback(
    async (code: string, trustDevice: boolean) => {
      const creds = pending.current;
      if (!creds) throw new SelfvoyError('bad_request', 'Начните вход заново');
      const device = await deviceInfo();
      // Без постоянного id «доверять устройству» нельзя: id сменится при перезапуске.
      const trust = trustDevice && device.persistent;
      const payload = await api.login2fa(creds.email, creds.password, code.trim().toUpperCase(), trust, deviceName(), device.id);
      await signedIn(payload);
    },
    [api, signedIn],
  );

  const register = useCallback(
    async (name: string, email: string, password: string, ref?: { code: string; source: string } | null): Promise<{ devCode?: string; done?: boolean }> => {
      const address = email.trim().toLowerCase();
      // 1. Попытка регистрации через сервер Selfvoy
      try {
        const result = await api.register(name.trim(), address, password, ref ?? undefined);
        pending.current = { email: address, password };
        setPendingEmail(address);
        if (!result.requires_verification) {
          if (result.user && 'token' in result && 'refresh_token' in result) {
            await signedIn({
              token: (result as any).token,
              refresh_token: (result as any).refresh_token,
              user: result.user,
            });
            return { done: true };
          }
          await login(address, password);
          return { done: true };
        }
        return { devCode: result.email_code };
      } catch (error) {
        // 2. Если основной сервер оффлайн, регистрируем напрямую в облачной базе Supabase Auth
        if (isSupabaseAvailable()) {
          const sbAuth = await supabaseSignUp(address, password, name.trim());
          const user = supabaseUserToSelfvoy(sbAuth.user);
          await signedIn({
            token: sbAuth.access_token,
            refresh_token: sbAuth.refresh_token,
            user,
          });
          return { done: true };
        }
        throw error;
      }
    },
    [api, login, signedIn],
  );

  const loginOAuth = useCallback(
    async (provider: string, code: string, state: string) => {
      const device = await deviceInfo();
      const payload = await api.oauthCallback(provider, code, state, deviceName(), device.id);
      await signedIn(payload);
    },
    [api, signedIn],
  );

  const verifyEmail = useCallback(
    async (code: string): Promise<LoginStep> => {
      const creds = pending.current;
      const email = creds?.email ?? pendingEmail;
      await api.verifyEmail(email, code);
      setPendingEmail('');
      // Пароль ещё в памяти — сразу входим, без повторного ввода.
      if (creds) return login(creds.email, creds.password);
      return { kind: 'done' };
    },
    [api, login, pendingEmail],
  );

  const resendCode = useCallback(async () => {
    const email = pending.current?.email ?? pendingEmail;
    const result = await api.resendCode(email);
    return { devCode: result.email_code };
  }, [api, pendingEmail]);

  const forgotPassword = useCallback(
    async (email: string) => {
      const result = await api.forgotPassword(email.trim().toLowerCase());
      return { devCode: result.reset_code };
    },
    [api],
  );

  const resetPassword = useCallback(
    async (email: string, code: string, newPassword: string) => {
      await api.resetPassword(email.trim().toLowerCase(), code, newPassword);
      return login(email, newPassword);
    },
    [api, login],
  );

  const changePassword = useCallback(
    async (oldPassword: string, newPassword: string) => {
      const result = await api.changePassword(oldPassword, newPassword);
      // Сервер отзывает все refresh-токены, включая наш, и выдаёт этому устройству новую
      // пару. Старые серверы её не присылают — тогда вход доживёт до истечения токена.
      if (result.token && result.refresh_token) {
        await withRefreshLock(() => saveTokens({ access: result.token!, refresh: result.refresh_token! }));
      }
    },
    [api],
  );

  const logout = useCallback(async () => {
    // Запрос уходит по текущему (авторизованному) соединению сразу, но его не ждём:
    // офлайн выход не должен висеть 25 секунд, токены удаляются немедленно.
    const farewell = api.logout(LOGOUT_TIMEOUT_MS);
    await signOutLocal(true);
    farewell.catch(() => {
      // Сервер не подтвердил выход — рвём соединение, чтобы оно не осталось авторизованным.
      socket.reconnect();
    });
  }, [api, socket, signOutLocal]);

  const setServer = useCallback(
    (input: string | null) => {
      const url = input ? normalizeServerUrl(input) : null;
      if (input && !url) return null;
      const env = currentEnv();
      const before = serverCandidates(overrideRef.current, env);
      const after = serverCandidates(url, env);
      saveOverride(url);
      if (sameServers(before, after)) {
        overrideRef.current = url;
        socket.reconnect();
        return after[0];
      }
      // Другой сервер: токены и кэш прежнего удаляем ДО подключения к новому —
      // токены сервера A не должны уйти на сервер B, лента пользователя A — достаться B.
      socket.stop();
      overrideRef.current = url;
      setEverConnected(false);
      void signOutLocal(true).finally(() => socket.reconnect());
      return after[0];
    },
    [socket, signOutLocal],
  );

  const value: Session = {
    api,
    socket,
    conn,
    everConnected,
    serverUrl,
    connError,
    status,
    user,
    pendingEmail,
    login,
    loginOAuth,
    login2fa,
    register,
    verifyEmail,
    resendCode,
    forgotPassword,
    resetPassword,
    changePassword,
    logout,
    setUser: applyUser,
    setServer,
    retry: () => socket.retryNow(),
  };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useSession(): Session {
  const value = useContext(Ctx);
  if (!value) throw new Error('useSession outside SessionProvider');
  return value;
}

/** Понятный текст ошибки для формы: сетевые — своими словами, остальное — как сказал сервер. */
export function errorText(error: unknown): string {
  if (error instanceof SelfvoyError) {
    switch (error.code) {
      case 'offline':
      case 'disconnected':
        return 'Нет связи с сервером Selfvoy';
      case 'timeout':
        return 'Сервер не ответил вовремя — попробуйте ещё раз';
      case 'conflict':
        return 'Этот email уже зарегистрирован';
      default: {
        const text = error.message || 'Что-то пошло не так';
        return text.charAt(0).toUpperCase() + text.slice(1);
      }
    }
  }
  return 'Что-то пошло не так';
}
