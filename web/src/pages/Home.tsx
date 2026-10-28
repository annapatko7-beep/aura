/*
  «Сегодня» — главный экран владельца: карточка следующей встречи и лента дня
  из настоящих данных (booking.list, tasks.list), с подтверждением и оплатой у
  каждой встречи и свободными окнами страницы записи (booking.get по своему
  slug — те же окна, что видит гость) с кнопкой «Предложить».
*/
import { Bell, BookOpen, Check, ChevronRight, Handshake, Plus, ShieldAlert, Star } from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { BookingSheet } from '../components/BookingSheet';
import { Avatar, Empty, Segmented, StatusGlyph, StatusPill } from '../components/ui';
import { SkeletonCard, SkeletonNextCard } from '../components/Skeleton';
import { pagePath, publicLink } from '../lib/booking';
import { canShare, copyText } from '../lib/clipboard';
import { shortWhen, toolTitle } from '../lib/format';
import { formatMoney } from '../lib/money';
import { selfvoyOf } from '../lib/negotiationSteps';
import { bookingMoneyLabel, paymentSummary } from '../lib/payment';
import { addDays, dayPhrase, hhmm, startOfDay } from '../lib/time';
import { buildDay, daySummary, nextUp, pageColor, untilText, type DayEntry, type FreeSlot } from '../lib/today';
import type { Booking } from '../net/bookingApi';
import { useData } from '../state/data';
import { useOwnerBookings } from '../state/owner';
import { useSession } from '../state/session';
import { NegotiationBadge } from './Negotiation';

const here = () => (typeof location === 'undefined' ? null : location);

export function Home() {
  const { user, conn } = useSession();
  const { tasks, confirmations, negotiations, unread, toggleTask, notify } = useData();
  const owner = useOwnerBookings();
  const { api, pages, bookings } = owner;
  const navigate = useNavigate();
  const [now, setNow] = useState(() => new Date());
  const [dayOffset, setDayOffset] = useState<'0' | '1'>('0');
  const [free, setFree] = useState<FreeSlot[]>([]);
  const [open, setOpen] = useState<{ id: number; start?: string } | null>(null);

  // Минутная стрелка: «через N мин» и линия «сейчас» живые.
  useEffect(() => {
    const timer = setInterval(() => setNow(new Date()), 30_000);
    return () => clearInterval(timer);
  }, []);

  // Свободные окна — с первой открытой страницы записи (ответ сервера кэшируется на минуту).
  const firstPage = (pages ?? []).find((p) => p.status === 'active');
  useEffect(() => {
    if (!firstPage || conn !== 'open') {
      setFree([]);
      return;
    }
    let alive = true;
    api
      .get(firstPage.slug)
      .then((reply) => {
        if (alive) setFree((reply.page.slots ?? []).map((s) => ({ start: s.start, end: s.end, pageId: firstPage.id })));
      })
      .catch(() => alive && setFree([]));
    return () => {
      alive = false;
    };
  }, [api, firstPage?.id, firstPage?.slug, conn]); // eslint-disable-line react-hooks/exhaustive-deps

  const day = useMemo(() => addDays(startOfDay(now), Number(dayOffset)), [now, dayOffset]);
  const entries = useMemo(() => buildDay({ bookings, tasks, free, pages }, day, now), [bookings, tasks, free, pages, day, now]);
  const summary = daySummary(entries, pages);
  const next = nextUp(bookings, now);
  const nextPage = next ? (pages ?? []).find((p) => p.id === next.booking.page_id) : undefined;
  const opened = open ? (bookings ?? []).find((b) => b.id === open.id) ?? null : null;

  const date = now.toLocaleDateString('ru-RU', { weekday: 'long', day: 'numeric', month: 'long' });
  const upcomingTasks = tasks
    .filter((t) => t.status === 'pending')
    .sort((a, b) => (a.due_at || a.remind_at || '9').localeCompare(b.due_at || b.remind_at || '9'))
    .slice(0, 5);

  const offer = async (entry: Extract<DayEntry, { kind: 'free' }>) => {
    const page = (pages ?? []).find((p) => p.id === entry.pageId);
    if (!page) return;
    const link = publicLink(pagePath(page.slug), page.url, here());
    const text = `Есть свободное окно ${dayPhrase(entry.start, now)} в ${hhmm(entry.start)} — ${page.title}.${link ? ` Записаться: ${link}` : ''}`;
    if (canShare()) {
      try {
        await navigator.share({ title: page.title, text });
        return;
      } catch (e) {
        if (e instanceof DOMException && e.name === 'AbortError') return;
      }
    }
    notify((await copyText(text)) ? 'Текст с окном скопирован — отправьте его клиенту' : 'Не удалось скопировать', 'info');
  };

  return (
    <div className="page wide">
      <div className="navrow">
        <span className="eyebrow">{date}</span>
        <span className="topbtns hide-desktop">
          <Link to="/inbox" className="cbtn glass" aria-label={`Входящие${unread + confirmations.length ? `: ${unread + confirmations.length}` : ''}`}>
            <Bell size={18} />
            {unread + confirmations.length > 0 && <span className="badge-dot" />}
          </Link>
          <Link to="/settings" aria-label="Настройки">
            <Avatar name={user?.display_name || user?.email || ''} src={user?.avatar_url} size={38} />
          </Link>
        </span>
      </div>
      <h1 className="title-xl" style={{ margin: '2px 0 16px' }}>
        Сегодня
      </h1>

      <div className="today-grid">
        <div className="stack" style={{ gap: 0 }}>
          {confirmations.length > 0 && (
            <Link to="/inbox" className="card row" style={{ gap: 12, marginBottom: 14 }}>
              <ShieldAlert size={20} style={{ color: 'var(--warning)' }} />
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ fontWeight: 600 }}>Ждут вашего решения: {confirmations.length}</div>
                <div className="small muted ellipsis">
                  {toolTitle(confirmations[0].tool)} · {confirmations[0].summary}
                </div>
              </div>
              <ChevronRight size={16} className="chev" />
            </Link>
          )}

          {next ? (
            <NextCard
              booking={next.booking}
              start={next.start}
              end={next.end}
              now={now}
              color={pageColor(next.booking.page_id, pages)}
              price={bookingMoneyLabel(next.booking, nextPage)}
              onOpen={() => setOpen({ id: next.booking.id, start: next.occurrence.start })}
            />
          ) : bookings === null ? (
            <SkeletonNextCard />
          ) : (
            <div className="next empty-next">
              <div className="lbl">
                <span>Следующая встреча</span>
              </div>
              <div className="title-md" style={{ marginTop: 6 }}>
                Предстоящих встреч нет
              </div>
              <div className="small muted" style={{ marginTop: 2 }}>
                {pages && pages.length === 0 ? 'Заведите страницу записи — клиенты будут записываться сами.' : 'Поделитесь ссылкой записи или постом со свободными окнами.'}
              </div>
              <div className="row wrap" style={{ marginTop: 12, gap: 8 }}>
                <button type="button" className="btn btn-primary btn-sm" onClick={() => navigate(pages && pages.length === 0 ? '/booking?new=1' : '/booking?post=1')}>
                  {pages && pages.length === 0 ? 'Создать страницу' : 'Пост со свободными окнами'}
                </button>
              </div>
            </div>
          )}

          <div className="sec-h">
            <h2>День</h2>
            <span>
              {summary.text}
              {summary.amount ? ` · ${formatMoney(summary.amount, summary.currency)}` : ''}
            </span>
          </div>
          <div style={{ marginBottom: 10 }}>
            <Segmented
              label="День"
              value={dayOffset}
              onChange={setDayOffset}
              options={[
                { value: '0', label: 'Сегодня' },
                { value: '1', label: 'Завтра' },
              ]}
            />
          </div>

          {entries.length === 0 ? (
            <div className="card">
              {bookings === null ? (
                <SkeletonCard lines={2} />
              ) : (
                <Empty>{dayOffset === '0' ? 'На сегодня ни встреч, ни задач со сроком.' : 'На завтра пока ничего нет.'}</Empty>
              )}
            </div>
          ) : (
            <div className="dl" role="list" aria-label="Лента дня">
              {entries.map((entry) => (
                <DayRow
                  key={entry.key}
                  entry={entry}
                  onOpen={(b, start) => setOpen({ id: b.id, start })}
                  onToggleTask={(id) => {
                    const task = tasks.find((t) => t.id === id);
                    if (task) void toggleTask(task);
                  }}
                  onOffer={offer}
                />
              ))}
            </div>
          )}
          {owner.error && <div className="notice error" style={{ marginTop: 12 }}>{owner.error}</div>}
        </div>

        <aside className="stack-lg" style={{ marginTop: 22 }}>
          <section>
            <div className="section-title">
              <h2 className="title-sm">Задачи</h2>
              <Link to="/tasks" className="link small">
                Все
              </Link>
            </div>
            <div className="card flush list">
              {upcomingTasks.length === 0 && <Empty>Задач нет. Скажите ассистенту, о чём напомнить.</Empty>}
              {upcomingTasks.map((t) => (
                <div key={t.id} className="list-row">
                  <button className="check" onClick={() => toggleTask(t)} aria-label={`Выполнено: ${t.title}`}>
                    <Check size={15} strokeWidth={3} />
                  </button>
                  <div className="body ellipsis">{t.title}</div>
                  <span className="meta">{shortWhen(t.due_at || t.remind_at)}</span>
                </div>
              ))}
            </div>
          </section>
          <section>
            <div className="section-title">
              <h2 className="title-sm">Переговоры</h2>
              <Link to="/negotiations" className="link small">
                Все
              </Link>
            </div>
            <div className="card flush list">
              {negotiations.length === 0 && <Empty>Попросите ассистента договориться о встрече — переговоры появятся здесь.</Empty>}
              {negotiations.slice(0, 3).map((n) => (
                <Link key={n.id} to={`/negotiations/${n.id}`} className="list-row">
                  <Handshake size={18} className="faint" />
                  <div className="body">
                    <div className="ellipsis">{selfvoyOf(n.targetName)}</div>
                    <div className="small muted ellipsis">
                      {n.result.accepted && n.result.slot ? `${shortWhen(n.result.slot.start)}${n.result.place?.name ? ` · ${n.result.place.name}` : ''}` : 'Общего времени не нашлось'}
                    </div>
                  </div>
                  <NegotiationBadge accepted={n.result.accepted} booked={n.booked} />
                </Link>
              ))}
            </div>
          </section>
        </aside>
      </div>

      <BookingSheet booking={opened} start={open?.start} pages={pages} api={api} onClose={() => setOpen(null)} onChanged={(b) => {
        owner.putBooking(b);
        void owner.reload();
      }} />
    </div>
  );
}

function NextCard({ booking, start, end, now, color, price, onOpen }: { booking: Booking; start: Date; end: Date; now: Date; color: number; price: string; onOpen: () => void }) {
  const minutes = Math.round((end.getTime() - start.getTime()) / 60_000);
  // price — цена встречи на момент записи («По абонементу» у пакета); оплата онлайн — отдельно.
  const pay = paymentSummary(booking.payment);
  return (
    <div className={`next c${color}`}>
      <div className="lbl">
        <span>Следующая · {untilText(start, now, end)}</span>
        <span className="ellipsis" style={{ maxWidth: '45%' }}>
          {booking.page_title}
        </span>
      </div>
      <div className="big">{hhmm(start)}</div>
      <div className="who">
        {booking.guest_name || 'Гость'}{' '}
        <span>
          · {minutes} мин{price ? ` · ${price}` : ''}
        </span>
      </div>
      {pay && (
        <div className="pay">
          <span className="num">{pay.text}</span>
          {pay.status && (
            <>
              <span aria-hidden="true">·</span>
              <StatusGlyph icon={pay.status.icon} />
              <span>{pay.status.label.toLowerCase()}</span>
            </>
          )}
        </div>
      )}
      <div className="gb">
        <button type="button" onClick={onOpen}>
          Подробнее
        </button>
        {booking.guest_email ? (
          <a href={`mailto:${encodeURIComponent(booking.guest_email)}`}>Написать</a>
        ) : (
          <Link to="/booking">Все записи</Link>
        )}
      </div>
    </div>
  );
}

function DayRow({
  entry,
  onOpen,
  onToggleTask,
  onOffer,
}: {
  entry: DayEntry;
  onOpen: (booking: Booking, start: string) => void;
  onToggleTask: (id: number) => void;
  onOffer: (entry: Extract<DayEntry, { kind: 'free' }>) => void;
}) {
  if (entry.kind === 'now') {
    return (
      <div className="dl-now" role="listitem" aria-label={`Сейчас ${hhmm(entry.start)}`}>
        <span>{hhmm(entry.start)}</span>
        <i />
      </div>
    );
  }
  if (entry.kind === 'free') {
    return (
      <div className="dl-row free" role="listitem">
        <span className="dl-time">{hhmm(entry.start)}</span>
        <span className="dl-cap" aria-hidden="true">
          <Plus size={20} />
        </span>
        <span className="dl-text">
          <b>Свободно</b>
          <span>
            {hhmm(entry.start)}–{hhmm(entry.end)}
          </span>
        </span>
        <button type="button" className="chipbtn" onClick={() => onOffer(entry)}>
          Предложить
        </button>
      </div>
    );
  }
  if (entry.kind === 'task') {
    return (
      <div className="dl-row" role="listitem">
        <span className="dl-time">{hhmm(entry.start)}</span>
        <button type="button" className="dl-cap task" onClick={() => onToggleTask(entry.task.id)} aria-label={`Выполнено: ${entry.task.title}`}>
          <Check size={20} />
        </button>
        <span className="dl-text">
          <b>{entry.task.title}</b>
          <span>Задача</span>
        </span>
        <span />
      </div>
    );
  }
  const { booking, occurrence, color, status } = entry;
  const minutes = Math.round((entry.end.getTime() - entry.start.getTime()) / 60_000);
  const Icon = color === 3 ? Star : BookOpen;
  return (
    <button type="button" className="dl-row" role="listitem" onClick={() => onOpen(booking, occurrence.start)}>
      <span className="dl-time">{hhmm(entry.start)}</span>
      <span className={`dl-cap c${color}`} aria-hidden="true">
        <Icon size={20} />
      </span>
      <span className="dl-text">
        <b>{booking.guest_name || 'Гость'}</b>
        <span>
          {booking.page_title || 'Запись'} · {minutes} мин
        </span>
      </span>
      <StatusPill status={status} />
    </button>
  );
}
