/*
  Приглашение по ссылке — сторона владельца (страница «Переговоры»).

  Для людей без Selfvoy: сервер подбирает свободные окна владельца и выдаёт ссылку;
  гость открывает её в браузере (#/i/<токен>) и выбирает время. Токен сервер
  отдаёт один раз — сразу после создания, в списке его нет. Список обновляется
  при уведомлениях invite.* (гость выбрал время / отказался).
*/
import { Copy, Link2, Share2, X } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState, type FormEvent } from 'react';
import { Empty, Field, Segmented, Spinner } from '../components/ui';
import { SkeletonList } from '../components/Skeleton';
import { canShare, copyText } from '../lib/clipboard';
import { shortWhen, when, WINDOWS, plural } from '../lib/format';
import {
  chosenSlot,
  GUEST_NAME_MAX,
  hasErrors,
  inviteFormErrors,
  inviteLink,
  inviteStatus,
  NOTE_MAX,
  ownerErrorText,
  statusLabel,
  statusTone,
  TOPIC_MAX,
  type InviteFormErrors,
  type InviteLink,
} from '../lib/invite';
import { makeInviteApi, type Invite } from '../net/inviteApi';
import { isNetworkError } from '../net/socket';
import { useData } from '../state/data';
import { useSession } from '../state/session';

const DURATIONS = [
  { value: '30', label: '30 мин' },
  { value: '60', label: '1 час' },
  { value: '90', label: '1,5 часа' },
];

export function InviteSection() {
  const { socket, conn } = useSession();
  const { notify } = useData();
  const api = useMemo(() => makeInviteApi(socket), [socket]);
  const [invites, setInvites] = useState<Invite[] | null>(null);
  const [listError, setListError] = useState('');
  const [created, setCreated] = useState<{ topic: string; link: InviteLink } | null>(null);
  const seq = useRef(0);

  const load = useCallback(async () => {
    const mine = ++seq.current;
    try {
      const reply = await api.list();
      if (mine !== seq.current) return;
      setInvites(reply.invites ?? []);
      setListError('');
    } catch (error) {
      if (mine !== seq.current) return;
      // Сетевые ошибки не шумим: есть баннер «нет связи», список перезагрузится при переподключении.
      if (!isNetworkError(error)) setListError(ownerErrorText(error));
    }
  }, [api]);

  // Открытие страницы и каждое переподключение — свежий список.
  useEffect(() => {
    if (conn === 'open') void load();
  }, [conn, load]);

  // Гость выбрал время или отказался — сервер шлёт уведомление invite.*.
  useEffect(
    () =>
      socket.onEvent((event) => {
        if (event.event !== 'notification.new') return;
        const kind = String((event.payload as { kind?: unknown }).kind ?? '');
        if (kind.startsWith('invite.')) void load();
      }),
    [socket, load],
  );

  const onCreated = (invite: Invite, link: InviteLink) => {
    setCreated({ topic: invite.topic, link });
    seq.current += 1; // ответ списка, начатого до создания, не должен затереть новую строку
    setInvites((list) => [invite, ...(list ?? []).filter((x) => x.id !== invite.id)]);
  };

  const cancel = async (id: number) => {
    try {
      const reply = await api.cancel(id);
      seq.current += 1;
      setInvites((list) => (list ?? []).map((x) => (x.id === id ? reply.invite : x)));
    } catch (error) {
      notify(ownerErrorText(error), 'error');
      void load();
    }
  };

  return (
    <section className="stack">
      <div>
        <h2 className="title-sm section-title">Пригласить по ссылке</h2>
        <p className="muted small" style={{ margin: '0 2px' }}>
          Для тех, у кого нет Selfvoy: Selfvoy подберёт ваши свободные окна, человек откроет ссылку в браузере и выберет время — без регистрации.
        </p>
      </div>
      {created ? <CreatedLink topic={created.topic} link={created.link} onDone={() => setCreated(null)} /> : <NewInvite api={api} onCreated={onCreated} />}

      <div className="card flush list">
        {invites === null && !listError && (
          <SkeletonList rows={2} />
        )}
        {listError && <div className="notice error" style={{ margin: 12 }}>{listError}</div>}
        {invites?.length === 0 && !listError && <Empty>Здесь появятся ваши приглашения и ответы гостей.</Empty>}
        {invites?.map((invite) => <InviteRow key={invite.id} invite={invite} onCancel={cancel} />)}
      </div>
    </section>
  );
}

function NewInvite({ api, onCreated }: { api: ReturnType<typeof makeInviteApi>; onCreated: (invite: Invite, link: InviteLink) => void }) {
  const [topic, setTopic] = useState('');
  const [guestName, setGuestName] = useState('');
  const [note, setNote] = useState('');
  const [windowHours, setWindowHours] = useState('168');
  const [duration, setDuration] = useState('60');
  const [errors, setErrors] = useState<InviteFormErrors & { form?: string }>({});
  const [busy, setBusy] = useState(false);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const next = inviteFormErrors({ topic, note, guestName });
    setErrors(next);
    if (hasErrors(next)) return;
    setBusy(true);
    try {
      const reply = await api.create({
        topic: topic.trim(),
        duration_minutes: Number(duration),
        window_hours: Number(windowHours),
        note: note.trim() || undefined,
        guest_name: guestName.trim() || undefined,
      });
      onCreated(reply.invite, inviteLink(reply.token, reply.url ?? '', typeof location === 'undefined' ? null : location));
      setTopic('');
      setGuestName('');
      setNote('');
    } catch (error) {
      setErrors({ form: ownerErrorText(error) });
    } finally {
      setBusy(false);
    }
  };

  return (
    <form className="card stack" onSubmit={submit} noValidate>
      {errors.form && <div className="notice error">{errors.form}</div>}
      <div className="grid-2">
        <Field label="Тема" error={errors.topic}>
          <input className={`input ${errors.topic ? 'invalid' : ''}`} placeholder="Например, кофе и обсудить проект" value={topic} onChange={(e) => setTopic(e.target.value)} maxLength={TOPIC_MAX} />
        </Field>
        <Field label="Для кого" error={errors.guestName} hint="Необязательно — гость сможет исправить">
          <input className={`input ${errors.guestName ? 'invalid' : ''}`} placeholder="Имя гостя" value={guestName} onChange={(e) => setGuestName(e.target.value)} maxLength={GUEST_NAME_MAX} />
        </Field>
      </div>
      <Field label="Заметка для гостя" error={errors.note} hint="Необязательно — гость увидит её на странице приглашения">
        <textarea className="textarea" style={{ minHeight: 64 }} placeholder="Где встречаемся, что взять с собой…" value={note} onChange={(e) => setNote(e.target.value)} maxLength={NOTE_MAX} />
      </Field>
      <div className="row wrap" style={{ gap: 16 }}>
        <div className="field">
          <span>Период</span>
          <Segmented label="Период" value={windowHours} onChange={setWindowHours} options={WINDOWS.map((w) => ({ value: String(w.hours), label: w.label }))} />
        </div>
        <div className="field">
          <span>Длительность</span>
          <Segmented label="Длительность" value={duration} onChange={setDuration} options={DURATIONS} />
        </div>
      </div>
      <div className="row wrap">
        <button className="btn btn-primary" type="submit" disabled={busy}>
          {busy ? (
            <>
              <Spinner small /> Подбираем окна…
            </>
          ) : (
            <>
              <Link2 size={16} /> Создать ссылку
            </>
          )}
        </button>
        <span className="hint">Гостю покажем только свободное время, без названий ваших событий.</span>
      </div>
    </form>
  );
}

function CreatedLink({ topic, link, onDone }: { topic: string; link: InviteLink; onDone: () => void }) {
  const [copied, setCopied] = useState(false);
  const [copyFailed, setCopyFailed] = useState(false);
  const text = link.kind === 'url' ? link.url : link.token;

  const copy = async () => {
    const ok = await copyText(text);
    setCopied(ok);
    setCopyFailed(!ok);
    if (ok) setTimeout(() => setCopied(false), 2000);
  };

  const share = async () => {
    if (link.kind !== 'url') return;
    try {
      await navigator.share({ title: 'Приглашение на встречу', text: `${topic} — выберите удобное время`, url: link.url });
    } catch {
      /* пользователь закрыл окно «Поделиться» */
    }
  };

  return (
    <div className="card stack" role="status">
      <div className="card-head" style={{ marginBottom: 0 }}>
        <h3 className="title-md">{link.kind === 'url' ? 'Ссылка готова' : 'Приглашение создано'}</h3>
        <button type="button" className="icon-btn" onClick={onDone} aria-label="Закрыть">
          <X size={16} />
        </button>
      </div>
      <div className="invite-link">
        <input className="input mono" readOnly value={text} aria-label={link.kind === 'url' ? 'Ссылка-приглашение' : 'Токен приглашения'} onFocus={(e) => e.currentTarget.select()} />
        <button type="button" className="btn btn-secondary" onClick={copy}>
          <Copy size={15} /> {copied ? 'Скопировано' : 'Скопировать'}
        </button>
        {link.kind === 'url' && canShare() && (
          <button type="button" className="btn btn-secondary" onClick={share}>
            <Share2 size={15} /> Поделиться
          </button>
        )}
      </div>
      {copyFailed && <div className="field-error">Не удалось скопировать — выделите текст и скопируйте вручную.</div>}
      {link.kind === 'token' && (
        <div className="notice warn">
          Сервер не знает свой публичный адрес, а это приложение открыто локально — готовую ссылку не собрать. Задайте на сервере SELFVOY_PUBLIC_URL (адрес веб-клиента), тогда ссылки будут приходить готовыми. Пока гостю можно открыть веб-клиент Selfvoy с адресом #/i/ и этим токеном.
        </div>
      )}
      <span className="hint">Ссылка показывается один раз — скопируйте её сейчас. В списке приглашений её не будет.</span>
    </div>
  );
}

function InviteRow({ invite, onCancel }: { invite: Invite; onCancel: (id: number) => Promise<void> }) {
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const status = inviteStatus(invite);
  const chosen = chosenSlot(invite);
  const guest = invite.guest_name.trim();
  const variants = invite.slots?.length ?? 0;

  let line: string;
  switch (status) {
    case 'accepted':
      line = [guest || 'Гость', chosen ? when(chosen.start) : ''].filter(Boolean).join(' · ');
      break;
    case 'declined':
      line = `${guest || 'Гость'}: не получится`;
      break;
    case 'cancelled':
      line = `Создано ${shortWhen(invite.created_at)}`;
      break;
    case 'expired':
      line = `Срок истёк ${shortWhen(invite.expires_at)}`;
      break;
    default:
      line = [guest ? `Для: ${guest}` : '', `ждёт ответа до ${shortWhen(invite.expires_at)}`, variants ? `${variants} ${plural(variants, 'вариант', 'варианта', 'вариантов')}` : '']
        .filter(Boolean)
        .join(' · ');
  }

  return (
    <div className="list-row invite-row">
      <div className="body">
        <div className="ellipsis">
          {invite.topic || 'Встреча'}
          <span className="muted"> · {invite.duration_minutes} мин</span>
        </div>
        <div className="small muted ellipsis">{line}</div>
        {invite.guest_email && (status === 'accepted' || status === 'declined') && <div className="small faint ellipsis">{invite.guest_email}</div>}
        {invite.guest_comment && <div className="small muted invite-comment">«{invite.guest_comment}»</div>}
      </div>
      {confirming ? (
        <div className="row" style={{ gap: 6 }}>
          <span className="small muted">Отменить?</span>
          <button
            className="btn btn-danger btn-sm"
            disabled={busy}
            onClick={async () => {
              setBusy(true);
              await onCancel(invite.id);
              setBusy(false);
              setConfirming(false);
            }}
          >
            {busy ? <Spinner small /> : 'Да'}
          </button>
          <button className="btn btn-ghost btn-sm" disabled={busy} onClick={() => setConfirming(false)}>
            Нет
          </button>
        </div>
      ) : (
        <div className="row" style={{ gap: 8 }}>
          <span className={`badge ${statusTone(status)}`}>{statusLabel(status)}</span>
          {status === 'open' && (
            <button className="btn btn-ghost btn-sm" onClick={() => setConfirming(true)}>
              Отменить
            </button>
          )}
        </div>
      )}
    </div>
  );
}
