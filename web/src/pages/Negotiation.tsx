import { ArrowLeft, CalendarCheck, CalendarClock, Check, ChevronRight, Clock, Handshake, MapPin, Plus, Star, X } from 'lucide-react';
import { useState, type FormEvent } from 'react';
import { Link, useNavigate, useParams, useSearchParams } from 'react-router-dom';
import { Avatar, Empty, Segmented, Spinner, StatusPill } from '../components/ui';
import { EmptyState } from '../components/EmptyState';
import { placeTags, shortWhen, when, WINDOWS } from '../lib/format';
import { selfvoyOf, negotiationSteps } from '../lib/negotiationSteps';
import { emailError } from '../lib/validate';
import { useData } from '../state/data';
import { useSession } from '../state/session';
import { InviteSection } from './Invites';

const DURATIONS = [
  { value: '30', label: '30 мин' },
  { value: '60', label: '1 час' },
  { value: '90', label: '1,5 часа' },
];

export function Negotiations() {
  const { negotiations } = useData();
  const [params] = useSearchParams();
  const [open, setOpen] = useState(params.get('new') === '1' || negotiations.length === 0);

  return (
    <div className="page">
      <header className="page-head">
        <div>
          <h1 className="title-xl">Переговоры</h1>
          <div className="sub">Ваш Selfvoy договаривается с Selfvoy других людей о времени и месте.</div>
        </div>
        {!open && (
          <button className="btn btn-primary" onClick={() => setOpen(true)}>
            <Plus size={16} /> Новые
          </button>
        )}
      </header>
      <div className="stack-lg">
        {open && <NewNegotiation onCancel={negotiations.length ? () => setOpen(false) : undefined} />}
        <section>
          <h2 className="title-sm section-title">История</h2>
          <div className="card flush list">
            {negotiations.length === 0 && (
              <EmptyState
                icon={Handshake}
                title="Переговоров ещё не было"
                desc="Ваш Selfvoy договорится о встрече с Selfvoy другого человека."
                action="Договориться"
                onAction={() => setOpen(true)}
              />
            )}
            {negotiations.map((n) => (
              <Link key={n.id} to={`/negotiations/${n.id}`} className="list-row">
                <Avatar name={n.targetName} size={38} />
                <div className="body">
                  <div className="ellipsis" style={{ fontWeight: 600 }}>
                    {selfvoyOf(n.targetName)}
                    {n.topic ? <span className="muted" style={{ fontWeight: 400 }}> · {n.topic}</span> : null}
                  </div>
                  <div className="small muted ellipsis">{n.result.accepted && n.result.slot ? when(n.result.slot.start) : `Запуск ${shortWhen(n.at)}`}</div>
                </div>
                <NegotiationBadge accepted={n.result.accepted} booked={n.booked} />
              </Link>
            ))}
          </div>
        </section>
        <InviteSection />
        <Link to="/booking" className="card row booking-teaser">
          <CalendarClock size={20} className="faint" />
          <span style={{ flex: 1, minWidth: 0 }}>
            <span className="title-sm" style={{ display: 'block' }}>Страница записи</span>
            <span className="small muted">Постоянная ссылка: клиенты сами выбирают свободное время, можно сразу серией.</span>
          </span>
          <ChevronRight size={18} className="chev" />
        </Link>
      </div>
    </div>
  );
}

function NewNegotiation({ onCancel }: { onCancel?: () => void }) {
  const { contacts, negotiate } = useData();
  const navigate = useNavigate();
  const [params] = useSearchParams();
  // ?to=<email> — переход из переписки «Договориться».
  const preset = contacts.find((c) => c.email === params.get('to')) ?? contacts[0];
  const [target, setTarget] = useState<{ email: string; name: string } | null>(preset ? { email: preset.email, name: preset.name } : null);
  const [email, setEmail] = useState('');
  const [topic, setTopic] = useState('');
  const [windowHours, setWindowHours] = useState('168');
  const [duration, setDuration] = useState('60');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const typed = email.trim().toLowerCase();
    // Введён email знакомого — берём его имя; иначе — часть до «@» («Selfvoy dima»).
    const known = contacts.find((c) => c.email.toLowerCase() === typed);
    const who = typed ? { email: typed, name: known?.name ?? typed.split('@')[0] } : target;
    if (!who) return setError('Выберите человека или введите email');
    if (typed && emailError(typed)) return setError('Похоже, в email опечатка');
    setError('');
    setBusy(true);
    const id = await negotiate(who.email, who.name, topic.trim(), Number(duration), Number(windowHours));
    setBusy(false);
    if (id) navigate(`/negotiations/${id}`);
  };

  return (
    <form className="card stack" onSubmit={submit}>
      <div className="card-head" style={{ marginBottom: 0 }}>
        <h2 className="title-md">Договориться о встрече</h2>
        {onCancel && (
          <button type="button" className="icon-btn" onClick={onCancel} aria-label="Закрыть">
            <X size={16} />
          </button>
        )}
      </div>
      <div className="field">
        <span>С кем</span>
        {contacts.length > 0 && (
          <div className="suggestions">
            {contacts.map((c) => (
              <button
                key={c.userId}
                type="button"
                className={`chip ${!email && target?.email === c.email ? 'active' : ''}`}
                onClick={() => {
                  setTarget({ email: c.email, name: c.name });
                  setEmail('');
                }}
              >
                <Avatar name={c.name} src={c.avatar} size={18} /> {c.name}
              </button>
            ))}
          </div>
        )}
        <input className="input" type="email" inputMode="email" placeholder={contacts.length ? 'или email собеседника' : 'email собеседника'} value={email} onChange={(e) => setEmail(e.target.value)} />
      </div>
      <label className="field">
        <span>Тема</span>
        <input className="input" placeholder="Например, обсудить стартап" value={topic} onChange={(e) => setTopic(e.target.value)} maxLength={120} />
      </label>
      <div className="row wrap" style={{ gap: 16 }}>
        <div className="field">
          <span>Когда</span>
          <Segmented label="Окно" value={windowHours} onChange={setWindowHours} options={WINDOWS.map((w) => ({ value: String(w.hours), label: w.label }))} />
        </div>
        <div className="field">
          <span>Длительность</span>
          <Segmented label="Длительность" value={duration} onChange={setDuration} options={DURATIONS} />
        </div>
      </div>
      {error && <div className="field-error">{error}</div>}
      <div className="row wrap">
        <button className="btn btn-primary" type="submit" disabled={busy}>
          {busy ? (
            <>
              <Spinner small /> Selfvoy договариваются…
            </>
          ) : (
            <>
              <Handshake size={16} /> Начать переговоры
            </>
          )}
        </button>
        <span className="hint">Selfvoy обеих сторон сверят свободное время; место — с учётом питания обоих. Бронь — только после вашего подтверждения.</span>
      </div>
    </form>
  );
}

export function NegotiationView() {
  const { id } = useParams();
  const { negotiations, book, negotiate } = useData();
  const { user } = useSession();
  const navigate = useNavigate();
  const [busy, setBusy] = useState(false);
  const n = negotiations.find((x) => x.id === id);

  if (!n) {
    return (
      <div className="page">
        <Empty>
          Переговоры не найдены. <Link className="link" to="/negotiations">К списку</Link>
        </Empty>
      </div>
    );
  }

  const r = n.result;
  const steps = negotiationSteps(n);
  const retryWider = async () => {
    setBusy(true);
    const next = await negotiate(n.target, n.targetName, n.topic, n.durationMinutes, 336);
    setBusy(false);
    if (next) navigate(`/negotiations/${next}`, { replace: true });
  };

  return (
    <div className="page">
      <Link to="/negotiations" className="btn btn-ghost btn-sm" style={{ marginLeft: -10, marginBottom: 12 }}>
        <ArrowLeft size={17} /> Переговоры
      </Link>
      <header className="page-head" style={{ alignItems: 'center' }}>
        <div className="row" style={{ gap: 12 }}>
          <span className="row" style={{ gap: 0 }}>
            <Avatar name={user?.display_name ?? 'Вы'} src={user?.avatar_url} size={40} />
            <span style={{ marginLeft: -8, borderRadius: '50%', boxShadow: '0 0 0 2px var(--bg)' }}>
              <Avatar name={n.targetName} size={40} />
            </span>
          </span>
          <div>
            <h1 className="title-lg">Ваш Selfvoy ↔ {selfvoyOf(n.targetName)}</h1>
            <div className="sub">{n.topic || 'Встреча'} · {n.durationMinutes} мин</div>
          </div>
        </div>
        <NegotiationBadge accepted={r.accepted} booked={n.booked} />
      </header>

      <div className="stack-lg">
        <section className="card outcome">
          {r.accepted && r.slot ? (
            <>
              <div className="title-sm">Итог</div>
              <div className="title-lg" style={{ textTransform: 'capitalize' }}>
                {when(r.slot.start)}
              </div>
              <div className="facts">
                <span>
                  <Clock size={16} /> {n.durationMinutes} мин
                </span>
                {r.place?.name && (
                  <span>
                    <MapPin size={16} /> {r.place.name}
                    {r.place.city ? `, ${r.place.city}` : ''}
                  </span>
                )}
                {r.place?.rating ? (
                  <span>
                    <Star size={16} /> {r.place.rating}
                  </span>
                ) : null}
                {r.place?.tags?.length ? <span>{placeTags(r.place.tags)}</span> : null}
              </div>
              {n.booked ? (
                <div className="status-line">
                  <Check size={17} style={{ color: 'var(--success)' }} /> Встреча в задачах{r.place?.name ? ', столик забронирован' : ''}.
                  <Link className="link" to="/tasks">
                    Открыть задачи
                  </Link>
                </div>
              ) : (
                <div className="row wrap">
                  <button
                    className="btn btn-primary"
                    disabled={busy}
                    onClick={async () => {
                      setBusy(true);
                      await book(n.id);
                      setBusy(false);
                    }}
                  >
                    {busy ? <Spinner small /> : <CalendarCheck size={17} />}
                    {r.place?.name ? 'Забронировать и добавить в задачи' : 'Добавить в задачи'}
                  </button>
                  <span className="hint">Бронирование — опасное действие: Selfvoy делает его только по вашей кнопке.</span>
                </div>
              )}
            </>
          ) : (
            <>
              <div className="title-sm">Итог</div>
              <div className="title-md">Общего времени в этом окне нет</div>
              <div className="row wrap">
                <button className="btn btn-secondary" onClick={retryWider} disabled={busy}>
                  {busy ? <Spinner small /> : null} Попробовать на две недели
                </button>
              </div>
            </>
          )}
        </section>

        <section>
          <h2 className="title-sm section-title">Как договорились</h2>
          <div className="card">
            <ol className="timeline" style={{ margin: 0, listStyle: 'none' }}>
              {steps.map((s, i) => (
                <li key={i} className={`tl-item ${s.state}`}>
                  <span className="mark">{s.state === 'ok' ? <Check size={13} strokeWidth={3} /> : s.state === 'fail' ? <X size={13} strokeWidth={3} /> : <span className="tiny">{i + 1}</span>}</span>
                  <div className="who">{s.actor}</div>
                  <div className="what">{s.text}</div>
                  {s.detail && <div className="detail">{s.detail}</div>}
                </li>
              ))}
            </ol>
          </div>
        </section>
      </div>
    </div>
  );
}

/** Итог переговоров — значок и слово. */
export function NegotiationBadge({ accepted, booked }: { accepted: boolean; booked?: boolean }) {
  if (booked) return <StatusPill status={{ label: 'В задачах', tone: 'green', icon: 'check' }} />;
  if (accepted) return <StatusPill status={{ label: 'Договорились', tone: 'accent', icon: 'check' }} />;
  return <StatusPill status={{ label: 'Нет общего окна', tone: 'amber', icon: 'x' }} />;
}
