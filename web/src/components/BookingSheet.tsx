/*
  Карточка записи владельца — окно снизу: кто, когда, сколько стоит, история
  (запись → оплата → напоминания → «Приду»), действия: написать, отменить
  встречу или всю серию, отметить оплату / «без оплаты» / возврат
  (booking.ownerCancel, booking.paymentSet). F5: копия напоминаний (cc_email) и
  отметка «По абонементу» со ссылкой на экран абонементов.
*/
import { Ban, Check, Mail, X } from 'lucide-react';
import { useState } from 'react';
import { Link } from 'react-router-dom';
import { bookingOwnerErrorText, bookingStatus, isUpcoming, sortedOccurrences } from '../lib/booking';
import { when } from '../lib/format';
import { packageSessionNote } from '../lib/packages';
import { bookingHistory, bookingMoney, bookingMoneyLabel, OWNER_PAYMENT_ACTION, ownerPaymentActions, paymentLabel, paymentSummary } from '../lib/payment';
import { occurrenceStatus } from '../lib/today';
import { dateLabel, dayTitle, hhmm } from '../lib/time';
import type { Booking, BookingApi, BookingPage, OwnerPaymentStatus } from '../net/bookingApi';
import { Avatar, Sheet, Spinner, StatusGlyph, StatusPill } from './ui';

function range(start: string, end: string): string {
  const a = new Date(start);
  const b = new Date(end);
  if (Number.isNaN(a.getTime())) return '';
  return Number.isNaN(b.getTime()) ? hhmm(a) : `${hhmm(a)}–${hhmm(b)}`;
}

export function BookingSheet({
  booking,
  start,
  pages,
  api,
  onClose,
  onChanged,
}: {
  booking: Booking | null;
  /** Встреча, с которой открыли карточку (серия); без неё — ближайшая. */
  start?: string;
  pages: BookingPage[] | null;
  api: BookingApi;
  onClose: () => void;
  onChanged: (booking: Booking) => void;
}) {
  const [busy, setBusy] = useState<string>('');
  const [error, setError] = useState('');
  const [confirming, setConfirming] = useState<'one' | 'all' | null>(null);

  const open = Boolean(booking);
  const close = () => {
    setConfirming(null);
    setError('');
    onClose();
  };
  if (!booking) return <Sheet open={false} onClose={close} label="Запись">{null}</Sheet>;

  const list = sortedOccurrences(booking.occurrences);
  const focus = (start && list.find((o) => o.start === start)) || list.find((o) => isUpcoming(o)) || list[list.length - 1];
  const page = (pages ?? []).find((p) => p.id === booking.page_id);
  const minutes = focus ? Math.round((Date.parse(focus.end) - Date.parse(focus.start)) / 60_000) : 0;
  const upcoming = list.filter((o) => isUpcoming(o));
  const status = bookingStatus(booking);
  const steps = bookingHistory(booking, focus?.start);
  const payment = booking.payment;
  const payView = payment ? paymentLabel(payment.status, 'owner') : null;
  // Цена встречи — снимок на момент записи (не payment.amount); оплата онлайн — отдельной строкой.
  const money = bookingMoney(booking, page);
  const moneyLabel = bookingMoneyLabel(booking, page);
  const pay = paymentSummary(payment);
  const actions = ownerPaymentActions(payment);
  const focusUpcoming = focus ? isUpcoming(focus) : false;

  const run = async (key: string, action: () => Promise<{ booking: Booking }>) => {
    setBusy(key);
    setError('');
    try {
      const reply = await action();
      onChanged(reply.booking);
      setConfirming(null);
    } catch (e) {
      setError(bookingOwnerErrorText(e));
    } finally {
      setBusy('');
    }
  };

  const setPayment = (status: OwnerPaymentStatus) => run(`pay-${status}`, () => api.paymentSet(booking.id, status));

  return (
    <Sheet
      open={open}
      onClose={close}
      leading={<Avatar name={booking.guest_name || '?'} size={52} />}
      title={booking.guest_name || 'Гость'}
      subtitle={[booking.page_title || 'Запись', minutes > 0 ? `${minutes} мин` : ''].filter(Boolean).join(' · ')}
    >
      {focus && (
        <div className="group-body">
          <div className="cell">
            <span className="lab">
              {(() => {
                const d = new Date(focus.start);
                return `${dayTitle(d)}, ${dateLabel(d)}`;
              })()}
              <small>
                {range(focus.start, focus.end)}
                {list.length > 1 ? ` · встреча ${list.indexOf(focus) + 1} из ${list.length}` : ''}
              </small>
            </span>
            <span className="val">
              {moneyLabel ? <span className={money.kind === 'money' ? 'num' : 'muted'}>{moneyLabel}</span> : null}
            </span>
          </div>
          <div className="cell">
            <span className="lab">Статус</span>
            <span className="row" style={{ gap: 6, flexWrap: 'wrap', justifyContent: 'flex-end' }}>
              {booking.status === 'active' && focus.status === 'active' ? (
                <StatusPill status={occurrenceStatus(booking, focus)} />
              ) : (
                <span className={`status-pill red`}>
                  <StatusGlyph icon="x" /> {status.label === 'Предстоит' ? 'Встреча отменена' : status.label}
                </span>
              )}
            </span>
          </div>
          {pay && (
            <div className="cell">
              <span className="lab">
                <span className="num">{pay.text}</span>
                <small>{payment?.method === 'yookassa' ? 'онлайн через ЮKassa' : 'по ссылке или реквизитам'}</small>
              </span>
              {payView && <StatusPill status={payView} />}
            </div>
          )}
          {Boolean(booking.package_id) && (
            <Link to={`/packages?id=${booking.package_id}`} className="cell" onClick={close}>
              <span className="lab">
                Оплата<small>{packageSessionNote(booking, focus)}</small>
              </span>
              <StatusPill status={{ label: 'По абонементу', tone: 'accent', icon: 'calendar' }} />
            </Link>
          )}
        </div>
      )}

      <div className="acts">
        <a
          href={booking.guest_email ? `mailto:${encodeURIComponent(booking.guest_email)}` : undefined}
          aria-disabled={!booking.guest_email}
          style={booking.guest_email ? undefined : { opacity: 0.45, pointerEvents: 'none' }}
        >
          <Mail size={22} /> Написать
        </a>
        {focusUpcoming && booking.status === 'active' && (
          <button type="button" className="danger" onClick={() => setConfirming('one')} disabled={Boolean(busy)}>
            <X size={22} /> {upcoming.length > 1 ? 'Отменить эту' : 'Отменить'}
          </button>
        )}
        {upcoming.length > 1 && booking.status === 'active' && (
          <button type="button" className="danger" onClick={() => setConfirming('all')} disabled={Boolean(busy)}>
            <Ban size={22} /> Отменить все
          </button>
        )}
      </div>

      {confirming && focus && (
        <div className="notice error" style={{ flexWrap: 'wrap', alignItems: 'center' }}>
          <span style={{ flex: '1 1 200px' }}>
            {confirming === 'one' ? `Отменить встречу ${when(focus.start)}? Гость получит письмо.` : `Отменить все предстоящие встречи (${upcoming.length})? Гость получит письмо.`}
            {payment?.status === 'paid' && confirming === 'all' ? ' Оплата вернётся гостю.' : ''}
          </span>
          <button
            type="button"
            className="btn btn-sm btn-primary"
            style={{ background: 'var(--danger)' }}
            disabled={Boolean(busy)}
            onClick={() => run('cancel', () => api.ownerCancel(booking.id, confirming === 'one' ? focus.start : undefined))}
          >
            {busy === 'cancel' ? <Spinner small /> : 'Да, отменить'}
          </button>
          <button type="button" className="btn btn-sm btn-ghost" onClick={() => setConfirming(null)}>
            Нет
          </button>
        </div>
      )}
      {error && <div className="notice error">{error}</div>}

      <div className="steps" aria-label="История записи">
        {steps.map((s) => (
          <div key={s.key} className={`step ${s.tone}`}>
            <span className="sdot" aria-hidden="true">
              <StatusGlyph icon={s.tone === 'done' ? 'check' : s.tone === 'wait' ? 'clock' : s.tone === 'fail' ? 'x' : 'bell'} size={14} />
            </span>
            <div>
              <b>{s.title}</b>
              {s.detail && <small>{s.detail}</small>}
            </div>
          </div>
        ))}
      </div>

      {list.length > 1 && (
        <div className="group">
          <div className="group-head">Встречи серии</div>
          <div className="group-body">
            {list.map((o) => (
              <div key={o.start} className={`cell occ ${o.status === 'cancelled' ? 'cancelled' : ''}`}>
                <span className="when">{when(o.start)}</span>
                {o.status === 'cancelled' ? (
                  <span className="status-pill red">
                    <StatusGlyph icon="x" /> Отменена
                  </span>
                ) : o.confirmed_at ? (
                  <span className="status-pill green">
                    <StatusGlyph icon="check" /> Придёт
                  </span>
                ) : null}
              </div>
            ))}
          </div>
        </div>
      )}

      {(booking.guest_email || booking.guest_comment || booking.cc_email) && (
        <div className="group-body">
          {booking.guest_email && (
            <div className="cell">
              <span className="lab">Email</span>
              <span className="val ellipsis" style={{ maxWidth: '65%' }}>
                {booking.guest_email}
              </span>
            </div>
          )}
          {booking.cc_email && (
            <div className="cell">
              <span className="lab">Копия</span>
              <span className="val ellipsis" style={{ maxWidth: '72%' }} title="Копия напоминаний: получает все письма о записи">
                {booking.cc_email}
              </span>
            </div>
          )}
          {booking.guest_comment && (
            <div className="cell stacked">
              <small className="muted">Комментарий</small>
              <span className="invite-comment">{booking.guest_comment}</span>
            </div>
          )}
        </div>
      )}

      {actions.length > 0 && (
        <div className="stack" style={{ gap: 8 }}>
          {actions.map((a, i) => (
            <button
              key={a}
              type="button"
              className={`btn btn-lg btn-block ${i === 0 && a === 'paid' ? 'btn-primary' : a === 'refunded' ? 'btn-danger' : 'btn-secondary'}`}
              disabled={Boolean(busy)}
              onClick={() => void setPayment(a)}
            >
              {busy === `pay-${a}` ? <Spinner small /> : a === 'paid' ? <Check size={18} /> : null} {OWNER_PAYMENT_ACTION[a]}
            </button>
          ))}
          {payment?.method === 'yookassa' && actions.includes('refunded') && (
            <span className="hint">Отметка «Возврат» не переводит деньги: вернуть оплату через ЮKassa можно, отменив запись целиком.</span>
          )}
        </div>
      )}
    </Sheet>
  );
}
