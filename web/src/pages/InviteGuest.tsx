/*
  Приглашение по ссылке — страница гостя (#/i/<токен>).

  Открывается без входа (и со входом тоже) вне оболочки приложения: никаких
  переадресаций на экран входа. invite.get / invite.respond — публичные методы,
  сокет отдаёт их и на неавторизованном соединении (onOpen без токенов просто
  ничего не восстанавливает). Слоты — по дням в часовом поясе браузера; после
  согласия — «Готово» и файл .ics, собранный здесь же, в браузере.
*/
import { CalendarPlus, Check, Clock, RefreshCw, WifiOff } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { Link, useParams } from 'react-router-dom';
import { Field, Logo, Spinner } from '../components/ui';
import { signupPath } from '../lib/booking';
import { genitive } from '../lib/format';
import {
  browserTimeZone,
  chosenSlot,
  COMMENT_MAX,
  EMAIL_MAX,
  GUEST_NAME_MAX,
  groupSlotsByDay,
  guestErrorKind,
  guestErrorText,
  guestFormErrors,
  guestIcs,
  hasErrors,
  icsFileName,
  inviteStatus,
  isPlausibleToken,
  slotPhrase,
  timeIn,
  timeZoneLabel,
  type GuestFormErrors,
} from '../lib/invite';
import { makeInviteApi, type GuestInvite, type InviteSlot } from '../net/inviteApi';
import { useSession } from '../state/session';

type Outcome = { action: 'accept'; slot: InviteSlot | null } | { action: 'decline' };

export function GuestLayout({ children }: { children: ReactNode }) {
  return (
    <main className="guest no-cta">
      <div className="g-head">
        <span className="logo-tile">
          <Logo size={30} />
        </span>
      </div>
      {children}
      <div className="auth-foot small">Selfvoy — личный ИИ-агент, который договаривается о встречах.</div>
    </main>
  );
}

export function Message({ title, children, action }: { title: string; children?: ReactNode; action?: ReactNode }) {
  return (
    <GuestLayout>
      <div className="stack">
        <h1 className="title-lg">{title}</h1>
        {children && <p className="lead" style={{ margin: 0 }}>{children}</p>}
        {action}
      </div>
    </GuestLayout>
  );
}

/**
  «Заведите свой Selfvoy» — или «Открыть Selfvoy», если человек уже вошёл. refCode —
  реферальный код владельца: регистрация по ссылке засчитается ему (referral.*).
*/
export function OwnSelfvoy({ refCode = '', source = 'invite' }: { refCode?: string; source?: 'invite' | 'booking' }) {
  const { status } = useSession();
  if (status === 'signedIn') {
    return (
      <div className="notice">
        <span>
          У вас уже есть Selfvoy —{' '}
          <Link className="link" to="/">
            открыть Selfvoy
          </Link>
          .
        </span>
      </div>
    );
  }
  return (
    <div className="notice">
      <span>
        Хотите, чтобы встречи согласовывались сами?{' '}
        <Link className="link" to={signupPath(refCode, source)}>
          Заведите свой Selfvoy
        </Link>
        .
      </span>
    </div>
  );
}

function downloadIcs(invite: GuestInvite, slot: InviteSlot): boolean {
  try {
    const blob = new Blob([guestIcs(invite, slot)], { type: 'text/calendar;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = icsFileName(slot);
    a.rel = 'noopener';
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10_000);
    return true;
  } catch {
    return false;
  }
}

function CalendarButton({ invite, slot }: { invite: GuestInvite; slot: InviteSlot }) {
  const [failed, setFailed] = useState(false);
  return (
    <>
      <button className="btn btn-primary btn-lg btn-block" onClick={() => setFailed(!downloadIcs(invite, slot))}>
        <CalendarPlus size={17} /> Добавить в календарь
      </button>
      {failed && <div className="field-error">Не удалось создать файл календаря.</div>}
    </>
  );
}

export function InviteGuest() {
  const { token } = useParams();
  const { socket, conn, connError, retry } = useSession();
  const api = useMemo(() => makeInviteApi(socket), [socket]);
  const zone = useMemo(browserTimeZone, []);

  const [invite, setInvite] = useState<GuestInvite | null>(null);
  const [missing, setMissing] = useState(!isPlausibleToken(token));
  const [loadError, setLoadError] = useState('');
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const outcomeRef = useRef<Outcome | null>(null);
  outcomeRef.current = outcome;
  const seq = useRef(0);
  const connRef = useRef(conn);
  connRef.current = conn;

  // Форма гостя.
  const [selected, setSelected] = useState<string | null>(null);
  const [name, setName] = useState('');
  const [email, setEmail] = useState('');
  const [comment, setComment] = useState('');
  const [errors, setErrors] = useState<GuestFormErrors & { form?: string }>({});
  const [busy, setBusy] = useState<'accept' | 'decline' | null>(null);
  const [confirmDecline, setConfirmDecline] = useState(false);
  const nameTouched = useRef(false);

  const apply = useCallback((next: GuestInvite) => {
    setInvite(next);
    // Подсказка владельца «для кого» — пока гость сам не начал печатать.
    if (!nameTouched.current && next.guest_name) setName(next.guest_name);
    // Выбранное время пропало из свежего списка — снимаем выбор.
    setSelected((cur) => (cur && (next.slots ?? []).some((s) => s.start === cur) ? cur : null));
  }, []);

  const load = useCallback(async () => {
    if (!isPlausibleToken(token)) return;
    const mine = ++seq.current;
    try {
      const reply = await api.get(token);
      if (mine !== seq.current || outcomeRef.current) return;
      setLoadError('');
      apply(reply.invite);
    } catch (error) {
      if (mine !== seq.current) return;
      const kind = guestErrorKind(error);
      if (kind === 'notFound') setMissing(true);
      // Сеть: экран «нет связи» по состоянию сокета; запрос повторится при переподключении.
      else if (kind !== 'network' || connRef.current === 'open') setLoadError(guestErrorText(error));
    }
  }, [api, token, apply]);

  // Первое подключение и каждое переподключение — свежие слоты (пока ответ не отправлен).
  useEffect(() => {
    if (conn === 'open' && !outcomeRef.current) void load();
  }, [conn, load]);

  const groups = useMemo(() => (invite ? groupSlotsByDay(invite.slots ?? [], zone) : []), [invite, zone]);

  const respond = async (action: 'accept' | 'decline') => {
    if (!invite || !token) return;
    const next = guestFormErrors({ name, email, comment, slot: selected }, action);
    setErrors(next);
    if (hasErrors(next)) {
      setConfirmDecline(false);
      return;
    }
    setBusy(action);
    try {
      const reply = await api.respond(token, {
        action,
        start: action === 'accept' ? selected ?? undefined : undefined,
        guest_name: name.trim(),
        guest_email: email.trim() || undefined,
        comment: comment.trim() || undefined,
      });
      seq.current += 1;
      setInvite(reply.invite);
      if (action === 'accept') {
        const picked = (invite.slots ?? []).find((s) => s.start === selected);
        setOutcome({ action, slot: chosenSlot(reply.invite) ?? picked ?? null });
      } else {
        setOutcome({ action });
      }
    } catch (error) {
      const kind = guestErrorKind(error);
      if (kind === 'notFound') {
        setMissing(true);
      } else if (kind === 'conflict') {
        // Закрыто, истекло или слот заняли: что именно — по свежему состоянию приглашения.
        try {
          const fresh = await api.get(token);
          seq.current += 1;
          apply(fresh.invite);
          setErrors(inviteStatus(fresh.invite) === 'open' ? { form: 'Это время уже заняли — выберите другое.' } : {});
        } catch {
          setErrors({ form: guestErrorText(error) });
        }
      } else {
        setErrors({ form: guestErrorText(error) });
      }
    } finally {
      setBusy(null);
      setConfirmDecline(false);
    }
  };

  // ------------------------------------------------------------ экраны

  if (missing) {
    return <Message title="Приглашение не найдено">Проверьте ссылку: возможно, она скопирована не полностью. Если всё верно — попросите прислать новую.</Message>;
  }

  if (outcome && invite) {
    if (outcome.action === 'decline') {
      return (
        <Message title="Ответ отправлен" action={<OwnSelfvoy refCode={invite.ref} />}>
          {invite.owner_name ? `${invite.owner_name} узнает, что в этот раз не получится.` : 'Организатор узнает, что в этот раз не получится.'}
        </Message>
      );
    }
    return (
      <GuestLayout>
        <div className="stack">
          <span className="invite-done" aria-hidden="true">
            <Check size={20} strokeWidth={2.5} />
          </span>
          <h1 className="title-lg">Готово</h1>
          <p className="lead" style={{ margin: 0 }}>
            {invite.owner_name ? `${invite.owner_name} получил(а) ваш ответ.` : 'Организатор получил ваш ответ.'}
          </p>
          {outcome.slot && (
            <>
              <div className="card stack" style={{ gap: 6 }}>
                <div className="title-md">{invite.topic || 'Встреча'}</div>
                <div className="muted">{slotPhrase(outcome.slot, zone)}</div>
                <div className="small faint">{timeZoneLabel(zone, new Date(outcome.slot.start))}</div>
              </div>
              <CalendarButton invite={invite} slot={outcome.slot} />
            </>
          )}
          <OwnSelfvoy refCode={invite.ref} />
        </div>
      </GuestLayout>
    );
  }

  if (invite) {
    const status = inviteStatus(invite);
    const owner = invite.owner_name || 'Организатор';
    // «у Ивана» / «у организатора» — родительный падеж (простые правила, как «Selfvoy Димы»).
    const ofOwner = invite.owner_name ? genitive(invite.owner_name) : 'организатора';
    if (status === 'accepted') {
      const chosen = chosenSlot(invite);
      return (
        <GuestLayout>
          <div className="stack">
            <h1 className="title-lg">Время уже выбрано</h1>
            <p className="lead" style={{ margin: 0 }}>
              На это приглашение уже ответили{invite.guest_name ? ` (${invite.guest_name})` : ''}.
            </p>
            {chosen && (
              <>
                <div className="card stack" style={{ gap: 6 }}>
                  <div className="title-md">{invite.topic || 'Встреча'}</div>
                  <div className="muted">{slotPhrase(chosen, zone)}</div>
                </div>
                <CalendarButton invite={invite} slot={chosen} />
              </>
            )}
          </div>
        </GuestLayout>
      );
    }
    if (status === 'declined') return <Message title="Приглашение закрыто">На это приглашение уже ответили отказом.</Message>;
    if (status === 'cancelled') return <Message title="Приглашение отменено">{`${owner} отменил(а) это приглашение.`}</Message>;
    if (status === 'expired') return <Message title="Срок приглашения истёк">{`Попросите новую ссылку у ${ofOwner}.`}</Message>;

    const ownerZoneDiffers = invite.owner_timezone && timeIn(new Date(), invite.owner_timezone) !== timeIn(new Date(), zone);

    return (
      <GuestLayout>
        <div>
          <h1 className="title-lg">
            {invite.owner_name ? `${invite.owner_name} приглашает: ` : 'Приглашение: '}
            {invite.topic || 'встреча'}
          </h1>
          <div className="facts" style={{ marginTop: 8 }}>
            <span>
              <Clock size={14} /> {invite.duration_minutes} мин
            </span>
          </div>
        </div>
        {invite.note && <div className="notice invite-note">{invite.note}</div>}
        {conn !== 'open' && <div className="notice warn">Нет связи с сервером Selfvoy — переподключаемся…</div>}

        {groups.length === 0 ? (
          <div className="notice">Свободного времени не осталось — напишите {invite.owner_name ? `организатору (${invite.owner_name})` : 'организатору'} напрямую.</div>
        ) : (
          <form className="stack" noValidate onSubmit={(e) => {
              e.preventDefault();
              void respond('accept');
            }}>
            {errors.form && <div className="notice error">{errors.form}</div>}
            <div className="field">
              <span>Выберите время</span>
              <div className="stack" style={{ gap: 14 }}>
                {groups.map((g) => (
                  <div key={g.key} role="group" aria-label={g.title}>
                    <div className="invite-day-title">{g.title}</div>
                    <div className="invite-slots">
                      {g.slots.map((s) => (
                        <button
                          key={s.start}
                          type="button"
                          className="invite-slot"
                          aria-pressed={selected === s.start}
                          onClick={() => {
                            setSelected(s.start);
                            setErrors((e) => ({ ...e, slot: undefined, form: undefined }));
                          }}
                        >
                          {s.label}
                        </button>
                      ))}
                    </div>
                  </div>
                ))}
              </div>
              {errors.slot ? (
                <span className="field-error">{errors.slot}</span>
              ) : (
                <span className="hint">
                  Время — в вашем часовом поясе: {timeZoneLabel(zone)}
                  {ownerZoneDiffers ? `. У ${ofOwner} — ${timeZoneLabel(invite.owner_timezone)}` : ''}
                </span>
              )}
            </div>
            <Field label="Ваше имя" error={errors.name}>
              <input
                className={`input ${errors.name ? 'invalid' : ''}`}
                autoComplete="name"
                value={name}
                maxLength={GUEST_NAME_MAX}
                onChange={(e) => {
                  nameTouched.current = true;
                  setName(e.target.value);
                }}
              />
            </Field>
            <Field label="Email" error={errors.email} hint="Необязательно — чтобы с вами могли связаться">
              <input className={`input ${errors.email ? 'invalid' : ''}`} type="email" inputMode="email" autoComplete="email" placeholder="you@example.com" value={email} maxLength={EMAIL_MAX} onChange={(e) => setEmail(e.target.value)} />
            </Field>
            <Field label="Комментарий" error={errors.comment} hint="Необязательно">
              <textarea className="textarea" style={{ minHeight: 64 }} value={comment} maxLength={COMMENT_MAX} onChange={(e) => setComment(e.target.value)} />
            </Field>
            <button className="btn btn-primary btn-lg btn-block" type="submit" disabled={busy !== null}>
              {busy === 'accept' ? <Spinner small /> : 'Подтвердить'}
            </button>
            {confirmDecline ? (
              <div className="row wrap" style={{ justifyContent: 'center' }}>
                <span className="small muted">Отказаться от встречи?</span>
                <button type="button" className="btn btn-danger btn-sm" disabled={busy !== null} onClick={() => void respond('decline')}>
                  {busy === 'decline' ? <Spinner small /> : 'Да, не получится'}
                </button>
                <button type="button" className="btn btn-ghost btn-sm" disabled={busy !== null} onClick={() => setConfirmDecline(false)}>
                  Назад
                </button>
              </div>
            ) : (
              <button type="button" className="btn btn-ghost btn-block" disabled={busy !== null} onClick={() => setConfirmDecline(true)}>
                Не получится
              </button>
            )}
          </form>
        )}
      </GuestLayout>
    );
  }

  if (loadError) {
    return (
      <Message
        title="Не удалось открыть приглашение"
        action={
          <button className="btn btn-secondary" onClick={() => void load()}>
            <RefreshCw size={15} /> Повторить
          </button>
        }
      >
        {loadError}
      </Message>
    );
  }

  if (conn !== 'open' && (conn === 'offline' || connError)) {
    return (
      <GuestLayout>
        <div className="stack">
          <div className="row">
            <WifiOff size={16} className="faint" />
            <h1 className="title-md">Нет связи с сервером Selfvoy</h1>
          </div>
          <p className="lead" style={{ margin: 0 }}>
            Проверьте интернет — мы пробуем подключиться снова автоматически.
          </p>
          <button className="btn btn-secondary" onClick={retry}>
            <RefreshCw size={15} /> Повторить
          </button>
        </div>
      </GuestLayout>
    );
  }

  return (
    <div className="gate">
      <div className="gate-card">
        <Spinner />
        <span className="muted small">Открываем приглашение…</span>
      </div>
    </div>
  );
}
