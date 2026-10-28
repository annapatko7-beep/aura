/*
  Настройки: профиль, предпочтения Selfvoy, приватность ИИ и разрешения,
  безопасность (пароль, 2FA, доверенные устройства, сессии), сервер и выход.
*/
import { AlertCircle, Check, ChevronRight, Copy, Download, KeyRound, LogOut, Monitor, RefreshCw, Share2, Shield, ShieldCheck, Smartphone, Sparkles, Trash2, Upload } from 'lucide-react';
import QRCode from 'qrcode';
import { Component, useCallback, useEffect, useMemo, useRef, useState, type FormEvent, type ReactNode } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { ServerAddress } from '../components/ServerAddress';
import { Avatar, Field, PasswordInput, Segmented, Spinner, Switch } from '../components/ui';
import { SkeletonProfile } from '../components/Skeleton';
import { bookingOwnerErrorText, publicLink, signupPath } from '../lib/booking';
import { DIETS, plural, toolDescription, toolTitle, when } from '../lib/format';
import { cleanCode, nameError, otpError, passwordError } from '../lib/validate';
import { isStaff } from '../net/adminApi';
import { makeBookingApi, type ReferralInfo } from '../net/bookingApi';
import { isNetworkError } from '../net/socket';
import { canShare, copyText } from '../lib/clipboard';
import { secureStore } from '../net/secureStore';
import { isNative } from '../platform';
import type { Permission, Privacy, SessionRow, TrustedDevice, TwoFaSetup } from '../net/api';
import { useData } from '../state/data';
import { errorText, useSession } from '../state/session';
import { DESIGN_OPTIONS, useTheme, type ThemeMode } from '../state/theme';
import { CalendarFeedSection, PaymentsSection, PlanCard, PushNotificationsSection, TelegramSection } from './Integrations';
import { planView } from '../lib/integrations';

const SECTIONS = [
  { id: 'profile', label: 'Профиль' },
  { id: 'appearance', label: 'Оформление' },
  { id: 'integrations', label: 'Подключения' },
  { id: 'selfvoy', label: 'Предпочтения' },
  { id: 'privacy', label: 'Приватность ИИ' },
  { id: 'security', label: 'Безопасность' },
  { id: 'referral', label: 'Пригласить друзей' },
  { id: 'app', label: 'Приложение' },
] as const;
type SectionId = (typeof SECTIONS)[number]['id'];

const isSection = (value: string | null): value is SectionId => SECTIONS.some((s) => s.id === value);

interface SectionBoundaryState {
  error: Error | null;
  key: string;
}

class SectionBoundary extends Component<{ children: ReactNode; sectionKey: string }, SectionBoundaryState> {
  override state: SectionBoundaryState = { error: null, key: this.props.sectionKey };

  static getDerivedStateFromProps(props: { sectionKey: string }, state: SectionBoundaryState): Partial<SectionBoundaryState> | null {
    return props.sectionKey === state.key ? null : { error: null, key: props.sectionKey };
  }

  static getDerivedStateFromError(error: Error): Partial<SectionBoundaryState> {
    return { error };
  }

  render() {
    const err = this.state.error;
    if (!err) return this.props.children;
    return (
      <div className="card notice error stack" style={{ gap: 12 }}>
        <div className="row" style={{ gap: 8 }}>
          <AlertCircle size={18} />
          <b>Не удалось отобразить этот раздел</b>
        </div>
        <p className="small muted" style={{ margin: 0 }}>
          {err.message || 'Произошла непредвиденная ошибка при загрузке данных раздела.'}
        </p>
        <div>
          <button className="btn btn-secondary btn-sm" type="button" onClick={() => this.setState({ error: null })}>
            <RefreshCw size={14} /> Попробовать снова
          </button>
        </div>
      </div>
    );
  }
}

export function Settings() {
  const [params, setParams] = useSearchParams();
  const asked = params.get('s');
  const [section, setSectionState] = useState<SectionId>(isSection(asked) ? asked : 'profile');
  // Переход по ссылке «Ещё» → «Подключения» на уже открытых настройках.
  useEffect(() => {
    if (isSection(asked)) setSectionState(asked);
  }, [asked]);
  const setSection = (id: SectionId) => {
    setSectionState(id);
    setParams(id === 'profile' ? {} : { s: id }, { replace: true });
  };
  return (
    <div className="page wide">
      <header className="page-head">
        <h1 className="title-xl">Настройки</h1>
      </header>
      <div className="settings">
        <nav className="settings-nav" aria-label="Разделы настроек">
          {SECTIONS.map((s) => (
            <button key={s.id} className={`nav-item ${section === s.id ? 'active' : ''}`} onClick={() => setSection(s.id)}>
              {s.label}
            </button>
          ))}
        </nav>
        <div className="stack-lg">
          <SectionBoundary sectionKey={section}>
            {section === 'profile' && (
              <>
                <ProfileSection />
                <PlanCard />
              </>
            )}
            {section === 'appearance' && <AppearanceSection />}
            {section === 'integrations' && (
              <>
                <PushNotificationsSection />
                <TelegramSection />
                <CalendarFeedSection />
                <PaymentsSection />
              </>
            )}
            {section === 'selfvoy' && <PrefsSection />}
            {section === 'privacy' && <PrivacySection />}
            {section === 'security' && (
              <>
                <PasswordSection />
                <TwoFactorSection />
                <SessionsSection />
              </>
            )}
            {section === 'referral' && <ReferralSection />}
            {section === 'app' && <AppSection />}
          </SectionBoundary>
        </div>
      </div>
    </div>
  );
}

function Card({ title, desc, children }: { title: string; desc?: ReactNode; children: ReactNode }) {
  return (
    <section className="card stack">
      <div>
        <h2 className="title-md">{title}</h2>
        {desc && <p className="small muted" style={{ margin: '4px 0 0' }}>{desc}</p>}
      </div>
      {children}
    </section>
  );
}

// ------------------------------------------------------------------ Профиль

/** Фото → квадрат 256 px JPEG (data URL): сервер принимает до ~100 КБ. */
async function resizeAvatar(file: File): Promise<string> {
  const url = URL.createObjectURL(file);
  try {
    const img = await new Promise<HTMLImageElement>((resolve, reject) => {
      const i = new Image();
      i.onload = () => resolve(i);
      i.onerror = () => reject(new Error('Не удалось открыть изображение'));
      i.src = url;
    });
    const side = Math.min(img.naturalWidth, img.naturalHeight);
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 256;
    const ctx = canvas.getContext('2d');
    if (!ctx) throw new Error('Не удалось обработать изображение');
    ctx.drawImage(img, (img.naturalWidth - side) / 2, (img.naturalHeight - side) / 2, side, side, 0, 0, 256, 256);
    return canvas.toDataURL('image/jpeg', 0.85);
  } finally {
    URL.revokeObjectURL(url);
  }
}

function ProfileSection() {
  const { api, user, setUser } = useSession();
  const { notify } = useData();
  const [name, setName] = useState(user?.display_name ?? '');
  const [bio, setBio] = useState(user?.bio ?? '');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const file = useRef<HTMLInputElement>(null);

  if (!user) return <SkeletonProfile />;
  const dirty = name.trim() !== (user.display_name ?? '') || bio.trim() !== (user.bio ?? '');

  const save = async (e: FormEvent) => {
    e.preventDefault();
    const problem = nameError(name);
    setError(problem);
    if (problem) return;
    setBusy(true);
    try {
      setUser((await api.updateProfile({ display_name: name.trim(), bio: bio.trim() })).user);
      notify('Профиль сохранён');
    } catch (err) {
      setError(errorText(err));
    } finally {
      setBusy(false);
    }
  };

  const setAvatar = async (avatar_url: string) => {
    try {
      setUser((await api.updateProfile({ avatar_url })).user);
    } catch (err) {
      notify(errorText(err), 'error');
    }
  };

  return (
    <Card title="Профиль" desc="Имя и фото видят люди, с которыми вы общаетесь, и их Selfvoy.">
      <div className="row" style={{ gap: 16 }}>
        <Avatar name={user.display_name || user.email} src={user.avatar_url} size={64} />
        <div className="row wrap">
          <button className="btn btn-secondary btn-sm" type="button" onClick={() => file.current?.click()}>
            <Upload size={14} /> Загрузить фото
          </button>
          {user.avatar_url && (
            <button className="btn btn-ghost btn-sm" type="button" onClick={() => setAvatar('')}>
              Убрать
            </button>
          )}
          <input
            ref={file}
            type="file"
            accept="image/png,image/jpeg,image/webp"
            hidden
            onChange={async (e) => {
              const f = e.target.files?.[0];
              e.target.value = '';
              if (!f) return;
              try {
                await setAvatar(await resizeAvatar(f));
              } catch (err) {
                notify(err instanceof Error ? err.message : 'Не удалось загрузить фото', 'error');
              }
            }}
          />
        </div>
      </div>
      <form className="stack" onSubmit={save} noValidate>
        <Field label="Имя" error={error}>
          <input className="input" value={name} onChange={(e) => setName(e.target.value)} maxLength={80} autoComplete="name" />
        </Field>
        <Field label="О себе" hint={`${[...bio].length}/300 — Selfvoy учитывает это в переговорах`}>
          <textarea className="textarea" value={bio} onChange={(e) => setBio(e.target.value.slice(0, 300))} placeholder="Чем занимаетесь, что любите" />
        </Field>
        <Field label="Email">
          <input className="input" value={user.email} disabled />
        </Field>
        <div>
          <button className="btn btn-primary" type="submit" disabled={!dirty || busy}>
            {busy ? <Spinner small /> : null} Сохранить
          </button>
        </div>
      </form>
    </Card>
  );
}

// ------------------------------------------------------------- Предпочтения

function PrefsSection() {
  const { prefs, savePrefs, notify } = useData();
  const [city, setCity] = useState(prefs?.city ?? '');
  const [budget, setBudget] = useState(prefs?.budget_limit ? String(prefs.budget_limit) : '');
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    setCity(prefs?.city ?? '');
    setBudget(prefs?.budget_limit ? String(prefs.budget_limit) : '');
  }, [prefs?.city, prefs?.budget_limit]);

  if (!prefs) return <Spinner />;
  const diet = new Set(prefs.diet ?? []);

  // От актуальных настроек, а не от снимка рендера: два быстрых клика не теряют первый.
  const toggleDiet = (value: string) =>
    savePrefs((current) => {
      const next = new Set(current.diet ?? []);
      if (next.has(value)) next.delete(value);
      else next.add(value);
      return { diet: [...next] };
    });

  const save = async (e: FormEvent) => {
    e.preventDefault();
    setBusy(true);
    const amount = Math.max(0, Math.round(Number(budget.replace(/\s/g, '')) || 0));
    const ok = await savePrefs({ city: city.trim(), budget_limit: amount });
    setBusy(false);
    if (ok) notify('Сохранено');
  };

  return (
    <Card title="Предпочтения" desc="Selfvoy учитывает их, когда выбирает место и время встречи.">
      <div className="field">
        <span>Питание</span>
        <div className="suggestions">
          {DIETS.map((d) => (
            <button key={d.value} type="button" className={`chip ${diet.has(d.value) ? 'active' : ''}`} onClick={() => toggleDiet(d.value)} aria-pressed={diet.has(d.value)}>
              {d.label}
            </button>
          ))}
        </div>
      </div>
      <form className="stack" onSubmit={save}>
        <div className="grid-2">
          <Field label="Город">
            <input className="input" value={city} onChange={(e) => setCity(e.target.value)} placeholder="Москва" autoComplete="address-level2" />
          </Field>
          <Field label="Средний чек на встречу, ₽" hint="0 — без ограничений">
            <input className="input" value={budget} onChange={(e) => setBudget(e.target.value.replace(/[^\d\s]/g, ''))} inputMode="numeric" placeholder="1500" />
          </Field>
        </div>
        <div>
          <button className="btn btn-primary" type="submit" disabled={busy}>
            {busy ? <Spinner small /> : null} Сохранить
          </button>
        </div>
      </form>
    </Card>
  );
}

// ------------------------------------------------------------- Приватность

const PRIVACY: { key: keyof Privacy; label: string; desc: string }[] = [
  { key: 'share_free_slots_only', label: 'Делиться только свободными слотами', desc: 'Чужой Selfvoy узнаёт, когда вы свободны, но не видит ваших заметок, дня рождения и аллергий.' },
  { key: 'allow_place_choice', label: 'Разрешить выбор локаций', desc: 'Selfvoy может предлагать место встречи. Если выключено — договаривается только о времени.' },
  { key: 'hide_event_titles', label: 'Скрывать названия других событий', desc: 'В переговорах ваши события видны как «занято», без названий.' },
];

const MODES: { value: Permission['mode']; label: string }[] = [
  { value: 'allow', label: 'Сама' },
  { value: 'ask', label: 'Спросить' },
  { value: 'deny', label: 'Нельзя' },
];

function PrivacySection() {
  const { prefs, savePrivacy, permissions, setPermission } = useData();
  const perms = permissions ?? [];
  return (
    <>
      <Card title="Приватность ИИ" desc="Чем ваш Selfvoy делится с Selfvoy других людей. Сервер применяет это при каждых переговорах.">
        {!prefs ? (
          <Spinner />
        ) : (
          <div>
            {PRIVACY.map((p) => (
              <div key={p.key} className="setting">
                <div className="body">
                  <div className="label">{p.label}</div>
                  <div className="desc">{p.desc}</div>
                </div>
                <Switch label={p.label} on={prefs.privacy?.[p.key] ?? true} onChange={(v) => savePrivacy({ [p.key]: v })} />
              </div>
            ))}
          </div>
        )}
      </Card>
      <Card title="Что Selfvoy делает сам" desc="Опасные действия по умолчанию — только с вашего подтверждения.">
        <div>
          {perms.map((p) => (
            <div key={p.tool} className="setting" style={{ flexWrap: 'wrap' }}>
              <div className="body">
                <div className="label">
                  {toolTitle(p.tool)} {p.dangerous && <span className="badge amber">важное</span>}
                </div>
                <div className="desc">{toolDescription(p.tool, p.description)}</div>
              </div>
              <Segmented label={toolTitle(p.tool)} value={p.mode || 'ask'} options={MODES} onChange={(mode) => setPermission(p.tool, mode)} />
            </div>
          ))}
          {perms.length === 0 && <div className="small muted">Нет доступных инструментов для настройки.</div>}
        </div>
      </Card>
    </>
  );
}

// ------------------------------------------------------------- Безопасность

function PasswordSection() {
  const { changePassword } = useSession();
  const { notify } = useData();
  const [current, setCurrent] = useState('');
  const [next, setNext] = useState('');
  const [errors, setErrors] = useState<{ current?: string | null; next?: string | null; form?: string }>({});
  const [busy, setBusy] = useState(false);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const found = { current: current ? null : 'Введите текущий пароль', next: passwordError(next) ?? (next === current ? 'Новый пароль совпадает с текущим' : null) };
    setErrors(found);
    if (found.current || found.next) return;
    setBusy(true);
    try {
      await changePassword(current, next);
      setCurrent('');
      setNext('');
      notify('Пароль изменён. Другие устройства вышли из аккаунта.');
    } catch (err) {
      setErrors({ form: errorText(err) });
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card title="Пароль" desc="После смены все другие устройства выйдут из аккаунта.">
      <form className="stack" onSubmit={submit} noValidate>
        {errors.form && <div className="notice error">{errors.form}</div>}
        <div className="grid-2">
          <Field label="Текущий пароль" error={errors.current}>
            <PasswordInput autoComplete="current-password" value={current} onChange={(e) => setCurrent(e.target.value)} invalid={Boolean(errors.current)} />
          </Field>
          <Field label="Новый пароль" error={errors.next} hint="8+ символов, буквы и цифры">
            <PasswordInput autoComplete="new-password" value={next} onChange={(e) => setNext(e.target.value)} invalid={Boolean(errors.next)} />
          </Field>
        </div>
        <div>
          <button className="btn btn-primary" type="submit" disabled={busy}>
            {busy ? <Spinner small /> : <KeyRound size={15} />} Сменить пароль
          </button>
        </div>
      </form>
    </Card>
  );
}

/** Секрет группами по 4 — так его проще перепечатать в приложение вручную. */
const grouped = (secret: string) => secret.replace(/(.{4})/g, '$1 ').trim();

/**
 * Сохранить текст файлом. В браузере и Electron — обычная загрузка (ссылку
 * освобождаем не сразу: загрузка начинается асинхронно). WebView телефона
 * загрузки по ссылке не поддерживает — там системное «Поделиться», если есть,
 * иначе кнопка не показывается (остаётся «Скопировать»).
 */
const canShareText = () => isNative && typeof navigator !== 'undefined' && typeof navigator.share === 'function';
const canDownload = () => !isNative || canShareText();

async function download(name: string, text: string): Promise<void> {
  if (isNative) {
    if (!canShareText()) throw new Error('Сохранение файла здесь недоступно — скопируйте коды');
    try {
      await navigator.share({ title: name, text });
    } catch (err) {
      if (err instanceof DOMException && err.name === 'AbortError') return; // пользователь закрыл окно
      throw err;
    }
    return;
  }
  const url = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.rel = 'noopener';
  a.style.display = 'none';
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

function TwoFactorSection() {
  const { api } = useSession();
  const { notify } = useData();
  const [status, setStatus] = useState<{ enabled: boolean; recovery_codes_left: number } | null>(null);
  const [setup, setSetup] = useState<(TwoFaSetup & { qr: string }) | null>(null);
  const [code, setCode] = useState('');
  const [codes, setCodes] = useState<string[] | null>(null);
  const [disabling, setDisabling] = useState(false);
  const [password, setPassword] = useState('');
  const [devices, setDevices] = useState<TrustedDevice[]>([]);
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const copy = (text: string, done: string) => {
    if (!navigator.clipboard) return notify('Буфер обмена недоступен', 'error');
    navigator.clipboard.writeText(text).then(
      () => notify(done),
      () => notify('Не удалось скопировать', 'error'),
    );
  };

  const load = () => {
    api.twoFaStatus().then(setStatus).catch(() => undefined);
    api
      .trustedDevices()
      .then((r) => setDevices(r.devices ?? []))
      .catch(() => setDevices([]));
  };
  useEffect(load, [api]); // eslint-disable-line react-hooks/exhaustive-deps

  const begin = async () => {
    setBusy(true);
    setError('');
    try {
      const s = await api.twoFaSetup();
      const qr = await QRCode.toDataURL(s.otpauth_uri, { margin: 0, width: 296, color: { dark: '#0b0c10', light: '#ffffff' } });
      setSetup({ ...s, qr });
    } catch (err) {
      setError(errorText(err));
    } finally {
      setBusy(false);
    }
  };

  const confirm = async (e: FormEvent) => {
    e.preventDefault();
    const problem = otpError(code);
    if (problem) return setError(problem);
    setBusy(true);
    try {
      const r = await api.twoFaConfirm(code);
      setCodes(r.recovery_codes);
      setSetup(null);
      setCode('');
      setError('');
      load();
    } catch (err) {
      setError(errorText(err));
    } finally {
      setBusy(false);
    }
  };

  const disable = async (e: FormEvent) => {
    e.preventDefault();
    if (!password) return setError('Введите пароль');
    setBusy(true);
    try {
      await api.twoFaDisable(password);
      setDisabling(false);
      setPassword('');
      setError('');
      notify('2FA отключена');
      load();
    } catch (err) {
      setError(errorText(err));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card
      title="Двухфакторная аутентификация"
      desc="При входе, кроме пароля, нужен код из Google Authenticator, Яндекс Ключа или другого TOTP-приложения."
    >
      {!status && <Spinner />}

      {codes && (
        <div className="stack">
          <div className="notice">Сохраните резервные коды — они показываются один раз. Каждый работает один раз, если телефона с приложением нет под рукой.</div>
          <div className="codes mono">
            {codes.map((c) => (
              <span key={c}>{c}</span>
            ))}
          </div>
          <div className="row wrap">
            <button className="btn btn-secondary btn-sm" onClick={() => copy(codes.join('\n'), 'Коды скопированы')}>
              <Copy size={14} /> Скопировать
            </button>
            {canDownload() && (
              <button
                className="btn btn-secondary btn-sm"
                onClick={() => download('selfvoy-recovery-codes.txt', codes.join('\n')).catch((err) => notify(err instanceof Error && err.message ? err.message : 'Не удалось сохранить файл', 'error'))}
              >
                {isNative ? <Share2 size={14} /> : <Download size={14} />} {isNative ? 'Поделиться' : 'Скачать'}
              </button>
            )}
            <button className="btn btn-primary btn-sm" onClick={() => setCodes(null)}>
              Я сохранил коды
            </button>
          </div>
        </div>
      )}

      {status && !status.enabled && !setup && !codes && (
        <div className="row wrap">
          <span className="badge">Выключена</span>
          <button className="btn btn-primary" onClick={begin} disabled={busy}>
            {busy ? <Spinner small /> : <ShieldCheck size={16} />} Включить 2FA
          </button>
          {error && <span className="field-error">{error}</span>}
        </div>
      )}

      {setup && (
        <form className="stack" onSubmit={confirm} noValidate>
          <ol className="small muted" style={{ margin: 0, paddingLeft: 18, display: 'grid', gap: 4 }}>
            <li>Откройте приложение-аутентификатор и добавьте аккаунт по QR-коду.</li>
            <li>Нет камеры? Введите ключ вручную (тип — по времени).</li>
            <li>Введите 6-значный код, который покажет приложение.</li>
          </ol>
          <div className="row wrap" style={{ gap: 20, alignItems: 'flex-start' }}>
            <img className="qr" src={setup.qr} alt="QR-код для приложения-аутентификатора" />
            <div className="stack" style={{ flex: '1 1 220px' }}>
              <div className="field">
                <span>Ключ</span>
                <div className="row">
                  <code className="secret mono" style={{ flex: 1 }}>
                    {grouped(setup.secret)}
                  </code>
                  <button type="button" className="icon-btn" aria-label="Скопировать ключ" onClick={() => copy(setup.secret, 'Ключ скопирован')}>
                    <Copy size={15} />
                  </button>
                </div>
              </div>
              <Field label="Код из приложения" error={error}>
                <input className={`input code-input mono ${error ? 'invalid' : ''}`} inputMode="numeric" autoComplete="one-time-code" value={code} onChange={(e) => setCode(cleanCode(e.target.value))} placeholder="000000" />
              </Field>
              <div className="row">
                <button className="btn btn-primary" type="submit" disabled={busy}>
                  {busy ? <Spinner small /> : null} Подтвердить и включить
                </button>
                <button
                  className="btn btn-ghost"
                  type="button"
                  onClick={() => {
                    setSetup(null);
                    setError('');
                  }}
                >
                  Отмена
                </button>
              </div>
            </div>
          </div>
        </form>
      )}

      {status?.enabled && !codes && (
        <div className="stack">
          <div className="row wrap">
            <span className="badge green">
              <ShieldCheck size={13} /> Включена
            </span>
            <span className="small muted">Резервных кодов осталось: {status.recovery_codes_left}</span>
            <span className="spacer" />
            {!disabling && (
              <button className="btn btn-danger btn-sm" onClick={() => setDisabling(true)}>
                Отключить
              </button>
            )}
          </div>
          {disabling && (
            <form className="row wrap" onSubmit={disable} noValidate style={{ alignItems: 'flex-end' }}>
              <div style={{ flex: '1 1 220px' }}>
                <Field label="Пароль для отключения" error={error}>
                  <PasswordInput autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} autoFocus />
                </Field>
              </div>
              <button className="btn btn-danger" type="submit" disabled={busy}>
                Отключить 2FA
              </button>
              <button className="btn btn-ghost" type="button" onClick={() => setDisabling(false)}>
                Отмена
              </button>
            </form>
          )}
          {devices.length > 0 && (
            <div className="stack" style={{ gap: 6 }}>
              <div className="title-sm">Доверенные устройства — вход без кода</div>
              <div className="card flush list" style={{ background: 'var(--bg)' }}>
                {devices.map((d) => (
                  <div key={d.id} className="list-row">
                    <Smartphone size={16} className="faint" />
                    <div className="body">
                      <div className="small">{d.device || 'Устройство'}</div>
                      <div className="tiny faint">до {when(d.expires_at)}</div>
                    </div>
                    <button
                      className="btn btn-ghost btn-sm"
                      onClick={async () => {
                        try {
                          await api.revokeTrustedDevice(d.id);
                          notify('Устройство больше не доверенное');
                        } catch (err) {
                          notify(errorText(err), 'error');
                        }
                        load();
                      }}
                    >
                      Забыть
                    </button>
                  </div>
                ))}
              </div>
            </div>
          )}
        </div>
      )}
    </Card>
  );
}

function SessionsSection() {
  const { api } = useSession();
  const { notify } = useData();
  const [sessions, setSessions] = useState<SessionRow[] | null>(null);

  const load = () => {
    api
      .sessions()
      .then((r) => setSessions(r.sessions ?? []))
      .catch(() => setSessions([]));
  };
  useEffect(load, [api]); // eslint-disable-line react-hooks/exhaustive-deps

  const others = (sessions ?? []).filter((s) => !s.current);
  return (
    <Card title="Активные сессии" desc="Устройства, где выполнен вход в ваш аккаунт.">
      {!sessions && <Spinner />}
      {sessions && (
        <div className="card flush list" style={{ background: 'var(--bg)' }}>
          {sessions.map((s) => (
            <div key={s.id} className="list-row">
              <Monitor size={16} className="faint" />
              <div className="body">
                <div className="small">
                  {s.device || 'Устройство'} {s.current && <span className="badge accent">это устройство</span>}
                </div>
                <div className="tiny faint">
                  {[
                    s.remote_addr && !s.remote_addr.startsWith(':') ? s.remote_addr : '',
                    s.created_at ? `вход ${when(s.created_at)}` : '',
                  ].filter(Boolean).join(' · ')}
                </div>
              </div>
              {!s.current && (
                <button
                  className="icon-btn"
                  aria-label="Завершить сессию"
                  onClick={async () => {
                    try {
                      await api.revokeSession(s.id);
                      notify('Сессия завершена');
                    } catch (err) {
                      notify(errorText(err), 'error');
                    }
                    load();
                  }}
                >
                  <Trash2 size={15} />
                </button>
              )}
            </div>
          ))}
        </div>
      )}
      {others.length > 0 && (
        <div>
          <button
            className="btn btn-danger btn-sm"
            onClick={async () => {
              try {
                await api.revokeOtherSessions();
                notify('Другие сессии завершены');
              } catch (err) {
                notify(errorText(err), 'error');
              }
              load();
            }}
          >
            Завершить все другие
          </button>
        </div>
      )}
    </Card>
  );
}

// -------------------------------------------------------------- Приложение

// ------------------------------------------------------------ Пригласить друзей

const SOURCE_LABEL: Record<string, string> = { link: 'по ссылке', booking: 'со страницы записи', invite: 'из приглашения' };

function ReferralSection() {
  const { socket, conn } = useSession();
  const api = useMemo(() => makeBookingApi(socket), [socket]);
  const [info, setInfo] = useState<ReferralInfo | null>(null);
  const [error, setError] = useState('');
  const [copied, setCopied] = useState(false);
  const seq = useRef(0);

  const load = useCallback(async () => {
    const mine = ++seq.current;
    try {
      const reply = await api.referral();
      if (mine !== seq.current) return;
      setInfo(reply);
      setError('');
    } catch (e) {
      if (mine === seq.current && !isNetworkError(e)) setError(bookingOwnerErrorText(e));
    }
  }, [api]);

  useEffect(() => {
    if (conn === 'open') void load();
  }, [conn, load]);

  // Кто-то зарегистрировался по ссылке — обновляем счётчики и список (как десктоп и Android).
  useEffect(
    () =>
      socket.onEvent((event) => {
        if (event.event !== 'notification.new') return;
        if (String((event.payload as { kind?: unknown }).kind ?? '') === 'referral.joined') void load();
      }),
    [socket, load],
  );

  // Ответы, пришедшие после ухода со страницы, не применяем.
  useEffect(
    () => () => {
      seq.current += 1;
    },
    [],
  );

  const link = info && info.code ? publicLink(signupPath(info.code), info.url ?? '', typeof location === 'undefined' ? null : location) : null;
  const referralsList = info?.referrals ?? [];
  const joinedCount = info?.joined ?? 0;
  const verifiedCount = info?.verified ?? 0;

  return (
    <Card title="Пригласить друзей" desc="Поделитесь ссылкой: коллега заведёт свой Selfvoy — и вы оба получите месяц Pro. Регистрации со страниц записи и приглашений тоже засчитываются вам.">
      {error && <div className="notice error">{error}</div>}
      {!info && !error && <Spinner small />}
      {info && (
        <>
          <div className="row wrap" style={{ gap: 12 }}>
            <span className="small muted">Ваш код</span>
            <span className="referral-code mono">{info.code}</span>
          </div>
          {link ? (
            <div className="invite-link">
              <input className="input mono" readOnly value={link} aria-label="Реферальная ссылка" onFocus={(e) => e.currentTarget.select()} />
              <button
                type="button"
                className="btn btn-secondary"
                onClick={async () => {
                  const ok = await copyText(link);
                  setCopied(ok);
                  if (ok) setTimeout(() => setCopied(false), 2000);
                }}
              >
                <Copy size={15} /> {copied ? 'Скопировано' : 'Скопировать'}
              </button>
              {canShare() && (
                <button type="button" className="btn btn-secondary" onClick={() => navigator.share({ title: 'Selfvoy', text: 'Заведи свой Selfvoy — личного ИИ-агента', url: link }).catch(() => undefined)}>
                  <Share2 size={15} /> Поделиться
                </button>
              )}
            </div>
          ) : (
            <div className="notice warn">Полную ссылку не собрать: задайте на сервере SELFVOY_PUBLIC_URL. Пока друг может ввести код при регистрации по адресу #/signup?ref={info.code}.</div>
          )}
          <div className="small muted">
            Зарегистрировались: {joinedCount} · подтвердили email: {verifiedCount}
          </div>
          {typeof info.reward_days === 'number' && (
            <div className="notice accent">
              <span>
                <b>+{info.reward_days} дней Pro вам и другу</b> — когда он подтвердит email. Наград получено: {info.rewarded ?? 0}
                {info.reward_cap ? ` из ${info.reward_cap}` : ''}.
                {planView(info.plan) ? ` Сейчас у вас: ${planView(info.plan)!.label}.` : ''}
              </span>
            </div>
          )}
          {referralsList.length > 0 && (
            <div className="list">
              {referralsList.map((r, i) => (
                <div key={`${r.joined_at || i}-${i}`} className="list-row">
                  <Avatar name={r.display_name || '?'} size={28} />
                  <div className="body">
                    <div className="ellipsis">{r.display_name || 'Без имени'}</div>
                    <div className="small muted">
                      {when(r.joined_at)} · {SOURCE_LABEL[r.source] ?? r.source}
                    </div>
                  </div>
                  <span className={`badge ${r.verified ? 'green' : ''}`}>{r.verified ? 'С Selfvoy' : 'Ждёт подтверждения'}</span>
                </div>
              ))}
            </div>
          )}
          {referralsList.length === 0 && <div className="small faint">Пока никто не зарегистрировался по вашей ссылке.</div>}
          {joinedCount > referralsList.length && (
            <div className="small faint">
              И ещё {joinedCount - referralsList.length} {plural(joinedCount - referralsList.length, 'человек', 'человека', 'человек')}.
            </div>
          )}
        </>
      )}
    </Card>
  );
}

function AppearanceSection() {
  const { theme, design, glassStyle, setTheme, setDesign, setGlassStyle } = useTheme();
  const { notify } = useData();

  const themeOptions: readonly { value: ThemeMode; label: string }[] = [
    { value: 'system', label: 'Как в системе' },
    { value: 'light', label: 'Светлая' },
    { value: 'dark', label: 'Тёмная' },
  ];

  return (
    <>
      <Card
        title="Тема оформления"
        desc="Выберите светлую, тёмную или автоматическую тему по системным настройкам вашего устройства."
      >
        <div style={{ maxWidth: 420 }}>
          <Segmented
            label="Тема оформления"
            value={theme}
            options={themeOptions}
            onChange={setTheme}
          />
        </div>
      </Card>

      <Card
        title="Стиль и варианты интерфейса"
        desc={`${DESIGN_OPTIONS.length} уникальных тем оформления на любой вкус: от неонового сияния iOS и киберпанка до изумрудного нефрита, полуночного сапфира, заката и строгого минимализма.`}
      >
        <div className="design-grid" role="radiogroup" aria-label="Вариант оформления">
          {DESIGN_OPTIONS.map((opt) => {
            const isSelected = design === opt.id;
            return (
              <button
                key={opt.id}
                type="button"
                role="radio"
                aria-checked={isSelected}
                className={`design-card ${isSelected ? 'active' : ''}`}
                onClick={() => {
                  setDesign(opt.id);
                  notify(`Тема «${opt.name}» успешно включена!`);
                }}
              >
                <div
                  className="design-swatch"
                  style={{
                    background:
                      opt.id === 'oled'
                        ? 'linear-gradient(135deg, #0d0b18 0%, #000000 85%)'
                        : opt.previewGradient,
                  }}
                >
                  <div className="swatch-top">
                    <div className="swatch-dots">
                      <span className="swatch-dot" />
                      <span className="swatch-dot" />
                      <span className="swatch-dot" />
                    </div>
                    <span className="swatch-accent-pill">
                      {opt.badgeLabel ?? opt.name}
                    </span>
                  </div>
                  <div
                    className="swatch-glass-button"
                    style={{
                      background: opt.buttonGradient ?? opt.previewGradient,
                    }}
                  >
                    <Sparkles size={13} />
                    <span>Кнопка</span>
                  </div>
                </div>
                <div className="design-info">
                  <div className="design-title-row">
                    <b>{opt.name}</b>
                    {isSelected && (
                      <span className="design-badge-active">
                        <Check size={14} strokeWidth={2.8} /> Выбрано
                      </span>
                    )}
                  </div>
                  <div className="design-tagline">{opt.tagline}</div>
                </div>
              </button>
            );
          })}
        </div>
      </Card>

      <Card
        title="Стиль кнопок и эффект жидкого стекла (iOS Liquid Glass)"
        desc="Кнопки созданы по принципам visionOS и iOS 18: полупрозрачное преломление света, зеркальный блик на верхней фаске (rim highlight), мягкие тени и упругий отклик на нажатие. Нажмите на любой стиль ниже, чтобы выбрать его и оценить тактильный эффект."
      >
        <div className="glass-showcase" role="radiogroup" aria-label="Стиль кнопок">
          <button
            type="button"
            role="radio"
            aria-checked={glassStyle === 'glow'}
            className={`btn btn-primary glass-choice-btn ${glassStyle === 'glow' ? 'selected' : ''}`}
            onClick={() => {
              setGlassStyle('glow');
              notify('Выбран стиль: Основная (Glow Glass) — неоновое сияние и блики');
            }}
          >
            <Sparkles size={16} />
            <span>Основная (Glow Glass)</span>
            {glassStyle === 'glow' && (
              <span className="glass-choice-badge">
                <Check size={12} strokeWidth={3} /> Выбрано
              </span>
            )}
          </button>
          <button
            type="button"
            role="radio"
            aria-checked={glassStyle === 'frosted'}
            className={`btn btn-secondary glass-choice-btn ${glassStyle === 'frosted' ? 'selected' : ''}`}
            onClick={() => {
              setGlassStyle('frosted');
              notify('Выбран стиль: Вторичная (Frosted Glass) — матовое бархатное стекло');
            }}
          >
            <span>Вторичная (Frosted Glass)</span>
            {glassStyle === 'frosted' && (
              <span className="glass-choice-badge">
                <Check size={12} strokeWidth={3} /> Выбрано
              </span>
            )}
          </button>
          <button
            type="button"
            role="radio"
            aria-checked={glassStyle === 'liquid'}
            className={`btn btn-glass glass-choice-btn ${glassStyle === 'liquid' ? 'selected' : ''}`}
            onClick={() => {
              setGlassStyle('liquid');
              notify('Выбран стиль: Полупрозрачная (Liquid Glass) — кристальное стекло iOS 18');
            }}
          >
            <span>Полупрозрачная (Liquid Glass)</span>
            {glassStyle === 'liquid' && (
              <span className="glass-choice-badge">
                <Check size={12} strokeWidth={3} /> Выбрано
              </span>
            )}
          </button>
        </div>
        <div className="small muted" style={{ marginTop: 10, lineHeight: 1.5 }}>
          💡 <b>Удобство для пожилых пользователей:</b> все кнопки увеличены до комфортной высоты от 44px до 52px (стандарт доступности Apple и WCAG AAA), контрастные подписи легко читаются без очков, а при нажатии срабатывает ощутимая физическая анимация сжатия.
        </div>
      </Card>
    </>
  );
}

function AppSection() {
  const { logout, user } = useSession();
  const [encrypted, setEncrypted] = useState<boolean | null>(null);
  useEffect(() => {
    secureStore.encrypted().then(setEncrypted, () => setEncrypted(false));
  }, []);
  const desktop = typeof window !== 'undefined' && Boolean(window.selfvoySecure);
  const storage =
    encrypted === null
      ? ''
      : encrypted
        ? 'Токены входа зашифрованы средствами системы (Keychain / DPAPI / libsecret).'
        : desktop
          ? 'В системе нет хранилища ключей (keyring) — токены входа лежат в профиле приложения без шифрования.'
          : 'Токены входа хранятся в песочнице приложения на этом устройстве. Пароль не сохраняется.';
  return (
    <>
      {isStaff(user?.role) && (
        // На телефоне в панели вкладок нет места: вход в админ-панель — здесь (на десктопе он в боковой панели).
        <Link to="/admin" className="card list-row admin-entry hide-desktop">
          <Shield size={18} strokeWidth={1.8} aria-hidden="true" />
          <span className="body">
            <span style={{ display: 'block', fontWeight: 500 }}>Администрирование</span>
            <span className="small muted" style={{ display: 'block' }}>
              Пользователи, блокировки{user?.role === 'admin' ? ', роли и журнал аудита' : ' и сессии'}
            </span>
          </span>
          <ChevronRight size={16} className="faint" aria-hidden="true" />
        </Link>
      )}
      <Card title="Сервер" desc="Адрес сервера Selfvoy, к которому подключено приложение.">
        <ServerAddress />
      </Card>
      <Card title="Хранение входа" desc={storage}>
        <div className="row small muted" style={{ gap: 8 }}>
          <ShieldCheck size={15} style={{ color: encrypted ? 'var(--success)' : 'var(--text-3)' }} />
          {encrypted ? 'Шифрование ОС включено' : 'Без шифрования ОС'}
        </div>
      </Card>
      <Card title="Правовая информация" desc="Условия обслуживания и политика обработки персональных данных.">
        <div className="stack" style={{ gap: 10 }}>
          <Link to="/privacy" target="_blank" className="link small row" style={{ gap: 6, alignItems: 'center' }}>
            <Shield size={14} /> Политика конфиденциальности
          </Link>
          <Link to="/terms" target="_blank" className="link small row" style={{ gap: 6, alignItems: 'center' }}>
            <ShieldCheck size={14} /> Условия использования и согласие 152-ФЗ
          </Link>
        </div>
      </Card>
      <Card title="Выход" desc="Токены удаляются с этого устройства; на других устройствах вход сохранится.">
        <div>
          <button className="btn btn-secondary" onClick={logout}>
            <LogOut size={15} /> Выйти из аккаунта
          </button>
        </div>
      </Card>
      <DeleteAccountCard />
    </>
  );
}

function DeleteAccountCard() {
  const { api, logout } = useSession();
  const [confirmOpen, setConfirmOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const handleDelete = async () => {
    setBusy(true);
    setError(null);
    try {
      await api.deleteAccount();
      await logout();
    } catch (err) {
      setError(errorText(err));
      setBusy(false);
    }
  };

  return (
    <Card
      title="Удаление аккаунта"
      desc="Полное и безвозвратное стирание вашего профиля, задач, интеграций, памяти ИИ и всех связанных данных в соответствии с требованиями Google Play, Apple App Store и ст. 14 152-ФЗ."
    >
      {error && <div className="notice error">{error}</div>}
      {!confirmOpen ? (
        <div>
          <button className="btn btn-danger" onClick={() => setConfirmOpen(true)}>
            <Trash2 size={15} /> Удалить мой аккаунт
          </button>
        </div>
      ) : (
        <div className="notice warn stack" style={{ gap: 12 }}>
          <div>
            <b>Вы уверены, что хотите безвозвратно удалить аккаунт?</b>
            <p className="small" style={{ margin: '6px 0 0', lineHeight: 1.45 }}>
              Все данные вашего профиля, активные сессии, подключенные календари, задачи и контекст общения с ИИ будут немедленно и окончательно удалены. Восстановить доступ будет невозможно.
            </p>
          </div>
          <div className="row" style={{ gap: 8, flexWrap: 'wrap' }}>
            <button className="btn btn-danger" disabled={busy} onClick={handleDelete}>
              {busy ? <Spinner small /> : 'Да, удалить аккаунт навсегда'}
            </button>
            <button className="btn btn-secondary" disabled={busy} onClick={() => setConfirmOpen(false)}>
              Отмена
            </button>
          </div>
        </div>
      )}
    </Card>
  );
}
