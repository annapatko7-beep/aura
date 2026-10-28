/*
  Админ-панель: обзор, пользователи (поиск, фильтры, блокировка, роли, сессии)
  и журнал аудита. Видна модераторам и администраторам; все права проверяет
  сервер (admin.*), интерфейс лишь прячет недоступные кнопки.
*/
import { Ban, ChevronLeft, LogOut, RefreshCw, Search, ShieldAlert, ShieldCheck, Undo2, UserCog } from 'lucide-react';
import { useCallback, useEffect, useId, useMemo, useRef, useState, type FormEvent, type KeyboardEvent, type ReactNode } from 'react';
import { Link } from 'react-router-dom';
import { Avatar, Empty, Field, Segmented, Spinner } from '../components/ui';
import { adminDate, auditLabel, auditSubline, count, isAuditAlert, pct } from '../lib/adminFormat';
import { shortWhen, when } from '../lib/format';
import { canModerate, isStaff, makeAdminApi, ROLE_LABEL, type AdminApi, type AdminStats, type AdminUser, type AdminUserDetail, type AuditEntry, type Role, type UserStatus } from '../net/adminApi';
import { useData } from '../state/data';
import { errorText, useSession } from '../state/session';

type Tab = 'overview' | 'users' | 'audit';

const PAGE_USERS = 50;
const PAGE_AUDIT = 100;

const ROLES: Role[] = ['user', 'moderator', 'admin'];

type RoleFilter = Role | 'all';
const ROLE_FILTERS: { value: RoleFilter; label: string }[] = [
  { value: 'all', label: 'Все роли' },
  { value: 'user', label: 'Пользователи' },
  { value: 'moderator', label: 'Модераторы' },
  { value: 'admin', label: 'Админы' },
];

type StatusFilter = Exclude<UserStatus, ''> | 'all';
const STATUS_FILTERS: { value: StatusFilter; label: string }[] = [
  { value: 'all', label: 'Любой статус' },
  { value: 'active', label: 'Активные' },
  { value: 'disabled', label: 'Заблокированные' },
  { value: 'unverified', label: 'Без подтверждения' },
];

export function Admin() {
  const { socket, user } = useSession();
  const role = user?.role;
  const api = useMemo(() => makeAdminApi(socket), [socket]);
  const [tab, setTab] = useState<Tab>('overview');

  if (!isStaff(role)) {
    return (
      <div className="page">
        <Empty>
          Раздел доступен только модераторам и администраторам. <Link to="/">На главную</Link>
        </Empty>
      </div>
    );
  }

  const tabs: { value: Tab; label: string }[] = [
    { value: 'overview', label: 'Обзор' },
    { value: 'users', label: 'Пользователи' },
  ];
  if (role === 'admin') tabs.push({ value: 'audit', label: 'Журнал' });

  return (
    <div className="page wide admin">
      <Link to="/settings" className="btn btn-ghost btn-sm hide-desktop admin-back">
        <ChevronLeft size={15} /> Настройки
      </Link>
      <header className="page-head">
        <div>
          <h1 className="title-xl">Администрирование</h1>
          <div className="sub">Вы — {ROLE_LABEL[role].toLowerCase()}. Действия записываются в журнал аудита.</div>
        </div>
      </header>
      <div className="stack-lg">
        <Segmented<Tab> label="Раздел администрирования" value={tab} options={tabs} onChange={setTab} />
        {tab === 'overview' && <Overview api={api} />}
        {tab === 'users' && <Users api={api} role={role} meId={user?.id ?? 0} />}
        {tab === 'audit' && role === 'admin' && <Audit api={api} />}
      </div>
    </div>
  );
}

// ------------------------------------------------------------------ общее

/** Загрузка с защитой от устаревших ответов: последний запрос побеждает, после размонтирования ответы игнорируются. */
function useRemote<T>(fetcher: () => Promise<T>) {
  const [state, setState] = useState<{ data: T | null; error: string; loading: boolean }>({ data: null, error: '', loading: true });
  const seq = useRef(0);
  const reload = useCallback(() => {
    const n = ++seq.current;
    setState((s) => ({ ...s, error: '', loading: true }));
    fetcher().then(
      (data) => {
        if (n === seq.current) setState({ data, error: '', loading: false });
      },
      (e: unknown) => {
        if (n === seq.current) setState((s) => ({ ...s, error: errorText(e), loading: false }));
      },
    );
  }, [fetcher]);
  useEffect(() => {
    reload();
    return () => {
      seq.current += 1;
    };
  }, [reload]);
  return { ...state, reload };
}

function Loading() {
  return (
    <div className="admin-loading">
      <Spinner />
    </div>
  );
}

function ErrorNotice({ text, onRetry }: { text: string; onRetry?: () => void }) {
  return (
    <div className="notice error admin-error" role="alert">
      <span className="spacer">{text}</span>
      {onRetry && (
        <button type="button" className="btn btn-ghost btn-sm" onClick={onRetry}>
          <RefreshCw size={14} /> Повторить
        </button>
      )}
    </div>
  );
}

/** Встроенное подтверждение опасного действия: Esc или «Отмена» — закрыть. */
function Confirm({ children, action, danger, busy, onConfirm, onCancel }: { children: ReactNode; action: string; danger?: boolean; busy: boolean; onConfirm: () => void; onCancel: () => void }) {
  const textId = useId();
  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    if (e.key === 'Escape' && !busy) {
      e.stopPropagation();
      onCancel();
    }
  };
  return (
    <div className="notice warn admin-confirm" role="alertdialog" aria-describedby={textId} onKeyDown={onKeyDown}>
      <span id={textId}>{children}</span>
      <div className="row wrap" style={{ gap: 8 }}>
        <button type="button" className={`btn btn-sm ${danger ? 'btn-danger' : 'btn-primary'}`} disabled={busy} onClick={onConfirm}>
          {busy && <Spinner small />} {action}
        </button>
        <button type="button" className="btn btn-ghost btn-sm" disabled={busy} onClick={onCancel} autoFocus>
          Отмена
        </button>
      </div>
    </div>
  );
}

// ------------------------------------------------------------------ обзор

function Overview({ api }: { api: AdminApi }) {
  const { data: stats, error, loading, reload } = useRemote<AdminStats>(api.stats);

  if (!stats) return error ? <ErrorNotice text={error} onRetry={reload} /> : <Loading />;
  const tiles: { label: string; value: string; extra?: string }[] = [
    { label: 'Пользователей', value: count(stats.users_total) },
    { label: 'Новых за 7 дней', value: count(stats.users_new_7d) },
    { label: 'Онлайн сейчас', value: count(stats.online_now) },
    { label: 'Активных сессий', value: count(stats.sessions_active) },
    { label: 'Подтвердили email', value: count(stats.users_verified), extra: pct(stats.users_verified, stats.users_total) },
    { label: 'С 2FA', value: count(stats.users_2fa), extra: pct(stats.users_2fa, stats.users_total) },
    { label: 'Заблокировано', value: count(stats.users_disabled) },
    { label: 'Модераторы / админы', value: `${count(stats.moderators)} / ${count(stats.admins)}` },
  ];
  return (
    <div className="stack">
      {error && <ErrorNotice text={error} onRetry={reload} />}
      <div className="admin-tiles" aria-busy={loading}>
        {tiles.map((t) => (
          <div key={t.label} className="card admin-tile">
            <div className="admin-tile-value">{t.value}</div>
            <div className="small muted">
              {t.label}
              {t.extra ? <span className="faint"> · {t.extra}</span> : null}
            </div>
          </div>
        ))}
      </div>
      <div>
        <button type="button" className="btn btn-ghost btn-sm" onClick={reload} disabled={loading}>
          {loading ? <Spinner small /> : <RefreshCw size={14} />} Обновить
        </button>
      </div>
    </div>
  );
}

// ------------------------------------------------------------- пользователи

type UserList = { users: AdminUser[]; total: number };

function Users({ api, role, meId }: { api: AdminApi; role: Role; meId: number }) {
  const [query, setQuery] = useState('');
  const [applied, setApplied] = useState('');
  const [roleFilter, setRoleFilter] = useState<RoleFilter>('all');
  const [status, setStatus] = useState<StatusFilter>('all');
  const [list, setList] = useState<UserList | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [openId, setOpenId] = useState<number | null>(null);
  const seq = useRef(0);
  const lastRequest = useRef<[number, number]>([0, PAGE_USERS]);

  /** offset = 0 — список заново, offset > 0 — дозагрузка следующей страницы. */
  const load = useCallback(
    (offset = 0, limit = PAGE_USERS) => {
      const n = ++seq.current;
      lastRequest.current = [offset, limit];
      setLoading(true);
      setError('');
      api.users(applied, roleFilter === 'all' ? '' : roleFilter, status === 'all' ? '' : status, offset, limit).then(
        (r) => {
          if (n !== seq.current) return;
          const users = r.users ?? [];
          setList((prev) => (offset && prev ? { users: [...prev.users, ...users], total: r.total } : { users, total: r.total }));
          setLoading(false);
        },
        (e: unknown) => {
          if (n !== seq.current) return;
          setError(errorText(e));
          setLoading(false);
        },
      );
    },
    [api, applied, roleFilter, status],
  );
  useEffect(() => {
    load(0);
    return () => {
      seq.current += 1;
    };
  }, [load]);

  const submit = (e: FormEvent) => {
    e.preventDefault();
    setApplied(query.trim());
  };

  if (openId !== null) {
    return (
      <UserDetail
        key={openId}
        api={api}
        role={role}
        meId={meId}
        userId={openId}
        onBack={(fresh) => {
          setOpenId(null);
          // Роль/статус могли поменяться: обновляем строку на месте, не теряя загруженные страницы.
          if (fresh) setList((prev) => prev && { ...prev, users: prev.users.map((x) => (x.id === fresh.id ? fresh : x)) });
        }}
      />
    );
  }

  const shown = list?.users.length ?? 0;
  return (
    <div className="stack">
      <form className="row wrap" role="search" onSubmit={submit} style={{ gap: 8 }}>
        <div className="input-icon" style={{ flex: '1 1 200px' }}>
          <Search size={15} className="faint" aria-hidden="true" />
          <input
            className="input"
            type="search"
            value={query}
            onChange={(e) => {
              setQuery(e.target.value);
              if (!e.target.value.trim() && applied) setApplied(''); // очистили поле — снова весь список
            }}
            placeholder="Имя или email"
            aria-label="Поиск пользователей"
            enterKeyHint="search"
          />
        </div>
        <button className="btn btn-secondary" type="submit">
          Найти
        </button>
      </form>
      <div className="row wrap admin-filters">
        <Segmented<RoleFilter> label="Фильтр по роли" value={roleFilter} onChange={setRoleFilter} options={ROLE_FILTERS} />
        <Segmented<StatusFilter> label="Фильтр по статусу" value={status} onChange={setStatus} options={STATUS_FILTERS} />
      </div>
      {error && <ErrorNotice text={error} onRetry={() => load(...lastRequest.current)} />}
      {!list && !error && <Loading />}
      {list && (
        <>
          <div className="small muted" aria-live="polite">
            {loading && !shown ? 'Загрузка…' : `Найдено: ${count(list.total)}`}
          </div>
          <div className="card flush list" aria-busy={loading}>
            {list.users.length === 0 && <Empty>{applied || roleFilter !== 'all' || status !== 'all' ? 'Никого не нашлось — попробуйте изменить фильтры.' : 'Пользователей пока нет.'}</Empty>}
            {list.users.map((u) => (
              <button key={u.id} type="button" className="list-row admin-user-row" onClick={() => setOpenId(u.id)}>
                <Avatar name={u.display_name || u.email} src={u.avatar_url || undefined} size={32} />
                <div className="body">
                  <div className="ellipsis" style={{ fontWeight: 500 }}>
                    {u.display_name || '—'}
                    {u.id === meId && <span className="faint"> · вы</span>}
                  </div>
                  <div className="small muted ellipsis">{u.email}</div>
                </div>
                <UserBadges user={u} />
              </button>
            ))}
          </div>
          {shown < list.total && (
            <div>
              <button type="button" className="btn btn-ghost btn-sm" disabled={loading} onClick={() => load(shown)}>
                {loading && <Spinner small />} Показать ещё ({count(list.total - shown)})
              </button>
            </div>
          )}
        </>
      )}
    </div>
  );
}

function UserBadges({ user }: { user: AdminUser }) {
  const badges: ReactNode[] = [];
  if (user.role !== 'user') badges.push(<span key="role" className={`badge ${user.role === 'admin' ? 'red' : 'accent'}`}>{ROLE_LABEL[user.role] ?? user.role}</span>);
  if (!user.is_active) badges.push(<span key="off" className="badge red">Заблокирован</span>);
  if (!user.email_verified) badges.push(<span key="unv" className="badge amber">Не подтверждён</span>);
  if (user.two_factor) badges.push(<span key="2fa" className="badge green">2FA</span>);
  if (!badges.length) return null;
  return <span className="admin-badges">{badges}</span>;
}

type Pending = { kind: 'block' } | { kind: 'revoke' } | { kind: 'role'; role: Role };

function UserDetail({ api, role, meId, userId, onBack }: { api: AdminApi; role: Role; meId: number; userId: number; onBack: (fresh?: AdminUser) => void }) {
  const { notify } = useData();
  const fetcher = useCallback(() => api.user(userId), [api, userId]);
  const { data: detail, error, loading, reload } = useRemote<AdminUserDetail>(fetcher);
  const [busy, setBusy] = useState(false);
  const [reason, setReason] = useState('');
  const [nextRole, setNextRole] = useState<Role | null>(null);
  const [pending, setPending] = useState<Pending | null>(null);

  const act = async (fn: () => Promise<string>) => {
    setBusy(true);
    try {
      notify(await fn());
      setReason('');
      setNextRole(null);
      setPending(null);
      reload();
    } catch (e) {
      notify(errorText(e), 'error');
    } finally {
      setBusy(false);
    }
  };

  const back = (
    <button type="button" className="btn btn-ghost btn-sm" onClick={() => onBack(detail?.user)} style={{ justifySelf: 'start' }}>
      <ChevronLeft size={15} /> К списку
    </button>
  );
  if (!detail) {
    return (
      <div className="stack">
        {back}
        {error ? <ErrorNotice text={error} onRetry={reload} /> : <Loading />}
      </div>
    );
  }

  const u = detail.user;
  const allowed = canModerate(role, meId, u);
  const who = u.display_name || u.email;
  const roleChoice = nextRole ?? u.role;
  const block = () => {
    const why = reason.trim();
    if (!why) return; // причина обязательна — сервер тоже проверит
    void act(async () => {
      await api.setActive(u.id, false, why);
      return 'Пользователь заблокирован, его сессии завершены';
    });
  };
  const unblock = () =>
    void act(async () => {
      await api.setActive(u.id, true, '');
      return 'Пользователь разблокирован';
    });
  const revoke = () =>
    void act(async () => {
      const r = await api.revokeSessions(u.id);
      return `Завершено сессий: ${count(r.revoked)}`;
    });
  const changeRole = (to: Role) =>
    void act(async () => {
      await api.setRole(u.id, to);
      return `Роль: ${ROLE_LABEL[to].toLowerCase()}`;
    });
  return (
    <div className="stack">
      {back}
      {error && <ErrorNotice text={error} onRetry={reload} />}
      <div className="card stack" aria-busy={loading}>
        <div className="row admin-user-head">
          <Avatar name={who} src={u.avatar_url || undefined} size={48} />
          <div className="body">
            <h2 className="title-md ellipsis" style={{ margin: 0 }}>
              {u.display_name || '—'}
            </h2>
            <div className="small muted ellipsis">{u.email}</div>
          </div>
          <UserBadges user={u} />
        </div>
        <dl className="admin-facts">
          <div>
            <dt>ID</dt>
            <dd>{u.id}</dd>
          </div>
          <div>
            <dt>Регистрация</dt>
            <dd>{adminDate(u.created_at) || '—'}</dd>
          </div>
          <div>
            <dt>Последний визит</dt>
            <dd>{u.last_seen_at ? adminDate(u.last_seen_at) : 'ещё не заходил(а)'}</dd>
          </div>
          <div>
            <dt>Активных сессий</dt>
            <dd>{count(detail.sessions_active)}</dd>
          </div>
        </dl>
        {u.id === meId && <div className="notice">Это вы. Свою роль и статус менять нельзя.</div>}
        {!allowed && u.id !== meId && <div className="notice">Модератор управляет только обычными пользователями.</div>}

        {allowed && (
          <div className="stack admin-actions">
            {u.is_active ? (
              <div className="stack" style={{ gap: 8 }}>
                <Field label="Причина блокировки">
                  <input
                    className="input"
                    value={reason}
                    onChange={(e) => setReason(e.target.value)}
                    maxLength={200}
                    placeholder="Обязательно — будет видна в журнале аудита"
                    disabled={busy || pending?.kind === 'block'}
                  />
                </Field>
                {pending?.kind === 'block' ? (
                  <Confirm danger busy={busy} action="Да, заблокировать" onCancel={() => setPending(null)} onConfirm={block}>
                    Заблокировать {who}? Пользователь не сможет войти, все его сессии будут завершены. Причина: «{reason.trim()}».
                  </Confirm>
                ) : pending?.kind === 'revoke' ? (
                  <Confirm busy={busy} action="Завершить" onCancel={() => setPending(null)} onConfirm={revoke}>
                    Завершить все сессии {who}? Пользователю придётся войти заново на всех устройствах.
                  </Confirm>
                ) : (
                  <div className="row wrap" style={{ gap: 8 }}>
                    <button type="button" className="btn btn-danger" disabled={busy || !reason.trim()} title={reason.trim() ? undefined : 'Укажите причину блокировки'} onClick={() => setPending({ kind: 'block' })}>
                      <Ban size={15} /> Заблокировать
                    </button>
                    <button type="button" className="btn btn-secondary" disabled={busy || detail.sessions_active === 0} onClick={() => setPending({ kind: 'revoke' })}>
                      <LogOut size={15} /> Завершить все сессии
                    </button>
                  </div>
                )}
              </div>
            ) : (
              <div className="row wrap" style={{ gap: 8 }}>
                <button type="button" className="btn btn-primary" disabled={busy} onClick={unblock}>
                  {busy ? <Spinner small /> : <Undo2 size={15} />} Разблокировать
                </button>
              </div>
            )}
            {role === 'admin' && (
              <div className="stack" style={{ gap: 8 }}>
                <div className="row wrap admin-role">
                  <Field label="Роль">
                    <select className="select" value={roleChoice} disabled={busy || pending?.kind === 'role'} onChange={(e) => setNextRole(e.target.value as Role)}>
                      {ROLES.map((r) => (
                        <option key={r} value={r}>
                          {ROLE_LABEL[r]}
                        </option>
                      ))}
                    </select>
                  </Field>
                  <button type="button" className="btn btn-secondary" disabled={busy || roleChoice === u.role || pending?.kind === 'role'} onClick={() => setPending({ kind: 'role', role: roleChoice })}>
                    <UserCog size={15} /> Сменить роль
                  </button>
                </div>
                {pending?.kind === 'role' && (
                  <Confirm
                    danger={pending.role === 'admin'}
                    busy={busy}
                    action="Сменить роль"
                    onCancel={() => {
                      setPending(null);
                      setNextRole(null);
                    }}
                    onConfirm={() => changeRole(pending.role)}
                  >
                    Сменить роль {who}: {ROLE_LABEL[u.role].toLowerCase()} → {ROLE_LABEL[pending.role].toLowerCase()}?
                    {pending.role === 'admin' && ' Администратор получит полный доступ к панели, ролям и журналу.'}
                  </Confirm>
                )}
              </div>
            )}
          </div>
        )}
      </div>

      <section>
        <div className="section-title">
          <h2 className="title-sm">Последние события</h2>
        </div>
        <AuditList entries={detail.audit ?? []} />
      </section>
    </div>
  );
}

// ---------------------------------------------------------------- журнал

function Audit({ api }: { api: AdminApi }) {
  const [entries, setEntries] = useState<AuditEntry[] | null>(null);
  const [error, setError] = useState('');
  const [more, setMore] = useState(false);
  const [busy, setBusy] = useState(false);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  const page = useCallback(
    async (before: number) => {
      setBusy(true);
      setError('');
      try {
        const r = await api.audit(0, before, PAGE_AUDIT);
        if (!alive.current) return;
        const next = r.entries ?? [];
        setEntries((prev) => (before && prev ? [...prev, ...next] : next));
        setMore(next.length === PAGE_AUDIT);
      } catch (e) {
        if (alive.current) setError(errorText(e));
      } finally {
        if (alive.current) setBusy(false);
      }
    },
    [api],
  );
  useEffect(() => {
    void page(0);
  }, [page]);

  const last = entries?.[entries.length - 1]?.id ?? 0;
  if (!entries) return error ? <ErrorNotice text={error} onRetry={() => void page(0)} /> : <Loading />;
  return (
    <div className="stack">
      <div>
        <button type="button" className="btn btn-ghost btn-sm" disabled={busy} onClick={() => void page(0)}>
          <RefreshCw size={14} /> Обновить
        </button>
      </div>
      <AuditList entries={entries} showUser />
      {error && <ErrorNotice text={error} onRetry={() => void page(last)} />}
      {more && !error && (
        <div>
          <button type="button" className="btn btn-ghost btn-sm" disabled={busy} onClick={() => void page(last)}>
            {busy && <Spinner small />} Показать ещё
          </button>
        </div>
      )}
    </div>
  );
}

function AuditList({ entries, showUser }: { entries: AuditEntry[]; showUser?: boolean }) {
  if (!entries.length) return <Empty>Событий пока нет.</Empty>;
  return (
    <ul className="card flush list admin-audit" aria-label="События журнала аудита">
      {entries.map((e) => {
        const alert = isAuditAlert(e.kind);
        const sub = auditSubline(e, showUser);
        const Icon = alert ? ShieldAlert : ShieldCheck;
        return (
          <li key={e.id} className="list-row">
            <Icon size={15} className={alert ? 'admin-alert' : 'faint'} aria-hidden="true" />
            <div className="body">
              <div className="ellipsis">{auditLabel(e.kind)}</div>
              {sub && (
                <div className="tiny faint ellipsis" title={sub}>
                  {sub}
                </div>
              )}
            </div>
            <time className="meta" dateTime={e.created_at} title={when(e.created_at)}>
              {shortWhen(e.created_at)}
            </time>
          </li>
        );
      })}
    </ul>
  );
}
