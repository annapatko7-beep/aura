/*
  Гостевые страницы записи (#/b/<slug>, #/m/<токен>): оболочка «как приложение»,
  календарь месяца со свободными днями, блок оплаты (F4), кнопка напоминаний в
  Telegram (F2) и подпись «Запись через Selfvoy» (F3). Грузятся отдельным
  чанком вместе с гостевыми страницами — без кода приложения владельца.
*/
import { ChevronLeft, ChevronRight, Copy, ExternalLink, Send } from 'lucide-react';
import { useMemo, useState, type ReactNode } from 'react';
import { Link } from 'react-router-dom';
import { signupPath } from '../lib/booking';
import { copyText } from '../lib/clipboard';
import { when } from '../lib/format';
import { formatMoney } from '../lib/money';
import { monthCells, monthOf, monthTitle, shiftMonth, WEEKDAYS } from '../lib/monthCalendar';
import { paymentLabel } from '../lib/payment';
import type { Branding, Payment } from '../net/bookingApi';
import { Avatar, Logo, Spinner, StatusPill } from './ui';

const host = () => (typeof location === 'undefined' ? '' : location.host);

/** Оболочка гостевой страницы: адрес сверху, контент, подпись; липкая кнопка снизу. */
export function GuestShell({ children, cta, footer }: { children: ReactNode; cta?: ReactNode; footer?: ReactNode }) {
  return (
    <>
      <main className={`guest ${cta ? '' : 'no-cta'}`}>
        <div className="g-site">{host() || 'Selfvoy'}</div>
        {children}
        {footer}
      </main>
      {cta && <div className="g-cta glass">{cta}</div>}
    </>
  );
}

export function GuestHead({ owner, subtitle }: { owner: string; subtitle?: string }) {
  return (
    <div className="g-head">
      <Avatar name={owner || 'Selfvoy'} size={72} />
      <b>{owner || 'Запись'}</b>
      {subtitle && <span>{subtitle}</span>}
    </div>
  );
}

/**
  Подпись «Запись через Selfvoy» (F3). Сервер решает, показывать ли её (branding.show:
  владелец с Pro может скрыть). Старый сервер без branding — показываем с реферальной
  ссылкой. url пуст (нет SELFVOY_PUBLIC_URL) — ссылка внутри этого веб-клиента.
*/
export function BrandingLink({ branding, refCode, compact }: { branding?: Branding; refCode?: string; compact?: boolean }) {
  if (branding && !branding.show) return null;
  const text = branding?.text || 'Запись через Selfvoy';
  const content = (
    <>
      <Logo size={compact ? 14 : 16} /> {text}
    </>
  );
  const url = branding?.url ?? '';
  const link = /^https?:\/\//i.test(url) ? (
    <a href={url} target="_blank" rel="noopener noreferrer" className="row" style={{ gap: 6, justifyContent: 'center' }}>
      {content}
    </a>
  ) : (
    <Link to={signupPath(refCode ?? '', 'booking')} className="row" style={{ gap: 6, justifyContent: 'center' }}>
      {content}
    </Link>
  );
  return compact ? <small>{link}</small> : <div className="g-brand">{link}</div>;
}

/** Календарь месяца: свободные дни выделены, по ним можно нажать. */
export function MonthCalendar({ freeDays, selected, onSelect, todayKey }: { freeDays: string[]; selected: string | null; onSelect: (key: string) => void; todayKey: string }) {
  const months = useMemo(() => [...new Set(freeDays.map(monthOf))].sort(), [freeDays]);
  const [month, setMonth] = useState(() => (selected ? monthOf(selected) : months[0] ?? monthOf(todayKey)));
  const free = useMemo(() => new Set(freeDays), [freeDays]);
  const first = months[0] ?? month;
  const last = months[months.length - 1] ?? month;
  const cells = monthCells(month);
  return (
    <div className="card mcal">
      <div className="mcal-head">
        <span>{monthTitle(month)}</span>
        <span className="row">
          <button type="button" className="icon-btn" aria-label="Предыдущий месяц" disabled={month <= first} onClick={() => setMonth(shiftMonth(month, -1))}>
            <ChevronLeft size={20} />
          </button>
          <button type="button" className="icon-btn" aria-label="Следующий месяц" disabled={month >= last} onClick={() => setMonth(shiftMonth(month, 1))}>
            <ChevronRight size={20} />
          </button>
        </span>
      </div>
      <div className="mcal-grid" role="grid" aria-label={monthTitle(month)}>
        {WEEKDAYS.map((w) => (
          <span key={w} className="w" aria-hidden="true">
            {w}
          </span>
        ))}
        {cells.map((cell, i) =>
          cell === null ? (
            <span key={`e${i}`} />
          ) : free.has(cell.key) ? (
            <button key={cell.key} type="button" className={`d ${cell.key === todayKey ? 'today' : ''}`} aria-pressed={selected === cell.key} aria-label={`${cell.day}, есть свободное время`} onClick={() => onSelect(cell.key)}>
              {cell.day}
            </button>
          ) : (
            <span key={cell.key} className={`d ${cell.key === todayKey ? 'today' : ''}`} aria-hidden="true">
              {cell.day}
            </span>
          ),
        )}
      </div>
    </div>
  );
}

/** «Напоминания в Telegram» (F2): подписка по ссылке на бота; подключено — отметка. */
export function TelegramBlock({ url, linked }: { url?: string; linked?: boolean }) {
  if (linked) {
    return (
      <div className="group-body">
        <div className="cell has-ico">
          <span className="ico tg">
            <Send size={16} />
          </span>
          <span className="lab">
            Напоминания в Telegram<small>Бот напомнит за сутки и за 2 часа</small>
          </span>
          <StatusPill status={{ label: 'Подключено', tone: 'green', icon: 'check' }} />
        </div>
      </div>
    );
  }
  if (!url) return null;
  return (
    <a className="btn btn-secondary btn-lg btn-block" href={url} target="_blank" rel="noopener noreferrer">
      <Send size={18} /> Напоминания в Telegram
    </a>
  );
}

/**
  Оплата записи для гостя (F4): сумма, срок, кнопка оплаты (action_url), реквизиты,
  «Я оплатил(а)» для оплаты по ссылке. После оплаты — статус.
*/
export function PaymentBlock({ payment, note, onClaim, claiming, checking }: { payment: Payment; note?: string; onClaim?: () => void; claiming?: boolean; checking?: boolean }) {
  const [copied, setCopied] = useState(false);
  const status = paymentLabel(payment.status, 'guest');
  const amount = formatMoney(payment.amount, payment.currency);
  const waiting = payment.status === 'pending';
  const what = payment.kind === 'deposit' ? 'Депозит' : payment.kind === 'package' ? 'Оплата пакета' : 'Предоплата';
  return (
    <div className="card pay-card" aria-live="polite">
      <div className="row" style={{ justifyContent: 'space-between', alignItems: 'flex-start' }}>
        <div>
          <div className="small muted">{what}</div>
          <div className="amount">{amount}</div>
        </div>
        {status && <StatusPill status={status} />}
      </div>
      {waiting && payment.due_at && <div className="small">Оплатите до {when(payment.due_at)} — иначе время освободится для других.</div>}
      {payment.status === 'claimed' && <div className="small muted">Специалист проверит поступление и подтвердит оплату.</div>}
      {checking && waiting && (
        <div className="row small muted">
          <Spinner small /> Проверяем оплату…
        </div>
      )}
      {waiting && payment.action_url && (
        <a className="btn btn-primary btn-lg btn-block" href={payment.action_url} target="_blank" rel="noopener noreferrer">
          Оплатить {amount} <ExternalLink size={16} />
        </a>
      )}
      {waiting && note && (
        <div className="row" style={{ gap: 8 }}>
          <span className="linkbox" style={{ flex: 1, fontFamily: 'var(--font)', fontSize: 14 }}>
            {note}
          </span>
          <button
            type="button"
            className="icon-btn"
            aria-label="Скопировать реквизиты"
            onClick={async () => {
              const ok = await copyText(note);
              setCopied(ok);
              if (ok) setTimeout(() => setCopied(false), 2000);
            }}
          >
            <Copy size={16} />
          </button>
          {copied && <span className="sr-only" role="status">Скопировано</span>}
        </div>
      )}
      {waiting && payment.method === 'link' && onClaim && (
        <button type="button" className={`btn btn-lg btn-block ${payment.action_url ? 'btn-secondary' : 'btn-primary'}`} disabled={claiming} onClick={onClaim}>
          {claiming ? <Spinner small /> : 'Я оплатил(а)'}
        </button>
      )}
    </div>
  );
}
