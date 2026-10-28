import { Bell, CalendarDays, Check, Clock, Eye, EyeOff, Hand, Minus, Plus, Undo2, X } from 'lucide-react';
import { useEffect, useId, useRef, useState, type InputHTMLAttributes, type KeyboardEvent, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import type { StatusIcon, StatusView } from '../lib/payment';

/**
  Знак Selfvoy: S-линия между двумя точками. Линия — фирменный градиент
  синий → фиолетовый → маджента, верхняя точка — цвет текста (на тёмном фоне —
  светлая), нижняя — янтарная #FFB224. mono — весь знак одним цветом (на цветной
  плитке).
*/
export function Logo({ size = 22, mono }: { size?: number; mono?: boolean }) {
  const id = `sv-grad-${useId().replace(/:/g, '')}`;
  return (
    <svg width={size} height={size} viewBox="0 0 100 100" fill="none" aria-hidden="true">
      {!mono && (
        <defs>
          <linearGradient id={id} x1="80" y1="20" x2="20" y2="80" gradientUnits="userSpaceOnUse">
            <stop offset="0" stopColor="#B04CFF" />
            <stop offset="0.5" stopColor="#6A4CFF" />
            <stop offset="1" stopColor="#3D6BFF" />
          </linearGradient>
        </defs>
      )}
      <path d="M76 28 C 38 24, 28 44, 50 50 C 72 56, 62 76, 24 72" stroke={mono ? 'currentColor' : `url(#${id})`} strokeWidth="11" strokeLinecap="round" />
      <circle cx="76" cy="28" r="10" fill="currentColor" />
      <circle cx="24" cy="72" r="10" fill={mono ? 'currentColor' : '#FFB224'} />
    </svg>
  );
}

// Мягкие оттенки для аватаров без фото — по имени, стабильно (тёмный текст на них читается).
const HUES = ['#CFE0FF', '#FFE2B3', '#D7F2E2', '#E9DDFF', '#FFD9E5', '#FFD8C7', '#D9F0F7'];

export function initials(name: string): string {
  const parts = name.trim().split(/\s+/).filter(Boolean);
  if (!parts.length) return '·';
  return (parts[0][0] + (parts[1]?.[0] ?? '')).toUpperCase();
}

export function Avatar({ name, src, size = 36 }: { name: string; src?: string; size?: number }) {
  const hue = HUES[[...name].reduce((sum, ch) => sum + ch.charCodeAt(0), 0) % HUES.length];
  return (
    <span className="avatar" role="img" aria-label={name} style={{ width: size, height: size, fontSize: size * 0.38, background: src ? undefined : hue }}>
      {src ? <img src={src} alt="" /> : initials(name)}
    </span>
  );
}

export function Spinner({ small }: { small?: boolean }) {
  return <span className={`spinner ${small ? 'sm' : ''}`} role="status" aria-label="Загрузка" />;
}

export function Field({ label, error, hint, children }: { label: string; error?: string | null; hint?: ReactNode; children: ReactNode }) {
  return (
    <label className="field">
      <span>{label}</span>
      {children}
      {error ? <span className="field-error">{error}</span> : hint ? <span className="hint">{hint}</span> : null}
    </label>
  );
}

export function PasswordInput(props: InputHTMLAttributes<HTMLInputElement> & { invalid?: boolean }) {
  const [shown, setShown] = useState(false);
  const { invalid, className, ...rest } = props;
  return (
    <span className="input-wrap">
      <input {...rest} type={shown ? 'text' : 'password'} className={`input ${invalid ? 'invalid' : ''} ${className ?? ''}`} />
      <button type="button" className="icon-btn" onClick={() => setShown((v) => !v)} aria-label={shown ? 'Скрыть пароль' : 'Показать пароль'}>
        {shown ? <EyeOff size={16} /> : <Eye size={16} />}
      </button>
    </span>
  );
}

export function Switch({ on, onChange, label, disabled }: { on: boolean; onChange: (v: boolean) => void; label: string; disabled?: boolean }) {
  return <button type="button" role="switch" aria-checked={on} aria-label={label} disabled={disabled} className={`switch ${on ? 'on' : ''}`} onClick={() => onChange(!on)} />;
}

/**
  Переключатель-радиогруппа. Клавиатура как у role="radiogroup": Tab попадает на
  выбранный вариант, стрелки и Home/End переводят фокус и выбор.
*/
export function Segmented<T extends string>({ value, options, onChange, label }: { value: T; options: readonly { value: T; label: string }[]; onChange: (v: T) => void; label: string }) {
  const selected = options.findIndex((o) => o.value === value);
  const tabStop = selected >= 0 ? selected : 0;
  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    const buttons = [...e.currentTarget.querySelectorAll<HTMLButtonElement>('button[role="radio"]')];
    const focused = buttons.indexOf(e.target as HTMLButtonElement);
    const from = focused >= 0 ? focused : tabStop;
    const last = options.length - 1;
    let next: number;
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') next = from >= last ? 0 : from + 1;
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') next = from <= 0 ? last : from - 1;
    else if (e.key === 'Home') next = 0;
    else if (e.key === 'End') next = last;
    else return;
    e.preventDefault();
    buttons[next]?.focus();
    if (options[next] && options[next].value !== value) onChange(options[next].value);
  };
  return (
    <div className="segmented" role="radiogroup" aria-label={label} onKeyDown={onKeyDown}>
      {options.map((o, i) => (
        <button
          key={o.value}
          type="button"
          role="radio"
          aria-checked={value === o.value}
          tabIndex={i === tabStop ? 0 : -1}
          className={`${value === o.value ? 'on' : ''} ${o.value}`}
          onClick={() => value !== o.value && onChange(o.value)}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

export function Empty({ children }: { children: ReactNode }) {
  return <div className="empty">{children}</div>;
}

const STATUS_ICONS: Record<StatusIcon, typeof Check> = {
  check: Check,
  clock: Clock,
  x: X,
  undo: Undo2,
  minus: Minus,
  hand: Hand,
  plus: Plus,
  bell: Bell,
  calendar: CalendarDays,
};

export function StatusGlyph({ icon, size = 14 }: { icon: StatusIcon; size?: number }) {
  const Icon = STATUS_ICONS[icon] ?? Check;
  return <Icon size={size} strokeWidth={2.4} aria-hidden="true" />;
}

/** Статус — всегда значок и слово (цвет только помогает). */
export function StatusPill({ status, className = '' }: { status: StatusView; className?: string }) {
  return (
    <span className={`status-pill ${status.tone} ${className}`}>
      <StatusGlyph icon={status.icon} />
      {status.label}
    </span>
  );
}

export function ProBadge() {
  return (
    <span className="badge pro" title="Функция тарифа Pro">
      PRO
    </span>
  );
}

/** Сгруппированный список в духе iOS: заголовок, белая подложка, подпись снизу. */
export function Group({ head, foot, children, id }: { head?: ReactNode; foot?: ReactNode; children: ReactNode; id?: string }) {
  return (
    <section className="group" id={id}>
      {head && <h2 className="group-head">{head}</h2>}
      <div className="group-body">{children}</div>
      {foot && <div className="group-foot">{foot}</div>}
    </section>
  );
}

let openSheets = 0;

/**
  Окно снизу (на телефоне — с крупными скруглениями и отступами от краёв, на
  десктопе — по центру). Escape и нажатие на затемнение закрывают; фокус уходит
  в окно и возвращается обратно; страница под окном не прокручивается.
*/
export function Sheet({
  open,
  onClose,
  title,
  subtitle,
  leading,
  children,
  label,
}: {
  open: boolean;
  onClose: () => void;
  title?: ReactNode;
  subtitle?: ReactNode;
  leading?: ReactNode;
  children: ReactNode;
  label?: string;
}) {
  const panel = useRef<HTMLDivElement>(null);
  const titleId = useId();
  const close = useRef(onClose);
  close.current = onClose;

  useEffect(() => {
    if (!open) return;
    const before = document.activeElement as HTMLElement | null;
    openSheets += 1;
    document.body.style.overflow = 'hidden';
    panel.current?.focus();
    const onKey = (e: globalThis.KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        close.current();
      }
    };
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('keydown', onKey);
      openSheets -= 1;
      if (openSheets === 0) document.body.style.overflow = '';
      before?.focus?.();
    };
  }, [open]);

  if (!open || typeof document === 'undefined') return null;
  return createPortal(
    <div className="sheet-root">
      <div className="sheet-dim" onClick={onClose} aria-hidden="true" />
      <div className="sheet" role="dialog" aria-modal="true" aria-label={title ? undefined : label} aria-labelledby={title ? titleId : undefined} tabIndex={-1} ref={panel}>
        <div className="sheet-scroll">
          <div className="sheet-grab" aria-hidden="true" />
          {(title || leading) && (
            <div className="sheet-head">
              {leading}
              <div className="sh-title">
                {title && <b id={titleId}>{title}</b>}
                {subtitle && <small>{subtitle}</small>}
              </div>
              <button type="button" className="close-btn" onClick={onClose} aria-label="Закрыть">
                <X size={16} strokeWidth={2.6} />
              </button>
            </div>
          )}
          {children}
        </div>
      </div>
    </div>,
    document.body,
  );
}
