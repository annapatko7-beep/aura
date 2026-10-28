/*
  Абонемент глазами гостя (#/p/<токен>, F5): открывается без входа, вне оболочки
  приложения (package.get / paymentClaim — публичные методы).

  Вид «как приложение»: кольцо остатка занятий, срок и статус, блок оплаты с
  «Я оплатил(а)» (оплата по ссылке), список записей по абонементу и липкая кнопка
  «Записаться по абонементу» — она ведёт на страницу записи с токеном пакета
  (#/b/<slug>?pkg=<токен>), и booking.book уходит с package_token. ?paid=1 —
  возврат со страницы оплаты ЮKassa: статус перечитывается несколько раз.
*/
import { CalendarPlus } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Link, useLocation, useParams } from 'react-router-dom';
import { BrandingLink, GuestHead, GuestShell, PaymentBlock } from '../components/guest';
import { StatusPill } from '../components/ui';
import { parseManageQuery } from '../lib/booking';
import { browserTimeZone, dayTitleIn, guestErrorKind, guestErrorText, isPlausibleToken, timeIn, timeZoneLabel } from '../lib/invite';
import { formatMoney } from '../lib/money';
import { canBookByPackage, packageBlockReason, packageBookPath, packageProgress, packageStatusView, ringDash, sessionsLabel, validityLabel } from '../lib/packages';
import type { StatusView } from '../lib/payment';
import { makePackageApi, type GuestPackage, type GuestPackageBooking } from '../net/packageApi';
import { useSession } from '../state/session';
import { guestScreens } from './BookingGuest';
import { Message } from './InviteGuest';

const PAID_POLLS = 4;
const PAID_POLL_MS = 4000;

/** Кольцо остатка: дуга — доля оставшихся занятий, в центре — сколько осталось. */
function Ring({ left, total }: { left: number; total: number }) {
  const r = 70;
  const { length, offset } = ringDash(left, total, r);
  return (
    <div className="pkg-ring" role="img" aria-label={`Осталось ${left} из ${total} занятий`}>
      <svg viewBox="0 0 168 168" width="168" height="168" aria-hidden="true">
        <circle cx="84" cy="84" r={r} className="track" />
        <circle cx="84" cy="84" r={r} className="arc" strokeDasharray={length} strokeDashoffset={offset} transform="rotate(-90 84 84)" />
      </svg>
      <div className="center">
        <b className="num">{left}</b>
        <small>из {total}</small>
      </div>
    </div>
  );
}

function bookingView(b: GuestPackageBooking, now: Date): StatusView {
  if (b.status === 'cancelled') return { label: 'Отменено', tone: 'red', icon: 'x' };
  return Date.parse(b.start) > now.getTime() ? { label: 'Предстоит', tone: 'accent', icon: 'calendar' } : { label: 'Прошло', tone: '', icon: 'check' };
}

export function PackageGuest() {
  const { token } = useParams();
  const location = useLocation();
  const { socket, conn, connError, retry } = useSession();
  const api = useMemo(() => makePackageApi(socket), [socket]);
  const zone = useMemo(browserTimeZone, []);
  const screens = guestScreens(conn, connError, retry);
  const query = useMemo(() => parseManageQuery(location.search), [location.search]);

  const [pkg, setPkg] = useState<GuestPackage | null>(null);
  const [missing, setMissing] = useState(!isPlausibleToken(token));
  const [loadError, setLoadError] = useState('');
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [claiming, setClaiming] = useState(false);
  const [checking, setChecking] = useState(query.paid);
  const seq = useRef(0);
  const polls = useRef(0);
  const connRef = useRef(conn);
  connRef.current = conn;

  const load = useCallback(async () => {
    if (!isPlausibleToken(token)) return;
    const mine = ++seq.current;
    try {
      const reply = await api.get(token);
      if (mine !== seq.current) return;
      setLoadError('');
      setPkg(reply.package);
    } catch (e) {
      if (mine !== seq.current) return;
      const kind = guestErrorKind(e);
      if (kind === 'notFound') setMissing(true);
      else if (kind !== 'network' || connRef.current === 'open') setLoadError(guestErrorText(e));
    }
  }, [api, token]);

  useEffect(() => {
    if (conn === 'open') void load();
  }, [conn, load]);

  // Вернулись со страницы оплаты (?paid=1): ЮKassa подтверждает не сразу — перечитываем несколько раз.
  useEffect(() => {
    if (!query.paid || conn !== 'open' || !pkg) return;
    if (pkg.payment?.status !== 'pending' || polls.current >= PAID_POLLS) {
      setChecking(false);
      return;
    }
    const timer = setTimeout(() => {
      polls.current += 1;
      void load();
    }, PAID_POLL_MS);
    return () => clearTimeout(timer);
  }, [query.paid, conn, pkg, load]);

  const claim = async () => {
    if (!token) return;
    setClaiming(true);
    setError('');
    try {
      const reply = await api.paymentClaim(token);
      seq.current += 1;
      setPkg(reply.package);
      setNotice('Спасибо! Специалист проверит оплату. Записываться по абонементу уже можно.');
    } catch (e) {
      if (guestErrorKind(e) === 'notFound') setMissing(true);
      else setError(guestErrorText(e));
      if (guestErrorKind(e) === 'conflict') void load();
    } finally {
      setClaiming(false);
    }
  };

  if (missing) {
    return <Message title="Абонемент не найден">Проверьте ссылку: возможно, она скопирована не полностью. Ссылка есть в письме о покупке.</Message>;
  }

  if (pkg && token) {
    const now = new Date();
    const { left, total, used } = packageProgress(pkg);
    const st = packageStatusView(pkg.status, 'guest');
    const can = canBookByPackage(pkg, now);
    const blocked = packageBlockReason(pkg, now);
    const owner = pkg.owner_name || 'Специалист';
    const bookings = [...(pkg.bookings ?? [])].filter((b) => !Number.isNaN(Date.parse(b.start))).sort((a, b) => Date.parse(b.start) - Date.parse(a.start));
    const upcoming = bookings.filter((b) => b.status === 'active' && Date.parse(b.start) > now.getTime()).length;
    const cta = can ? (
      <>
        <Link className="btn btn-primary btn-lg btn-block" to={packageBookPath(pkg.page_slug, token)}>
          <CalendarPlus size={18} /> Записаться по абонементу
        </Link>
        <small>
          Спишется одно занятие · осталось {sessionsLabel(left)}
        </small>
      </>
    ) : undefined;

    return (
      <GuestShell cta={cta} footer={<BrandingLink branding={pkg.branding} refCode={pkg.ref} />}>
        <GuestHead owner={pkg.owner_name} subtitle={pkg.page_title || 'Абонемент'} />

        <div className="card pkg-hero">
          <Ring left={left} total={total} />
          <div className="pkg-hero-text">
            <b>{pkg.title || 'Абонемент'}</b>
            <span className="small muted">
              Осталось {sessionsLabel(left)} · использовано {used} из {total}
            </span>
            <span className="row wrap" style={{ gap: 6, justifyContent: 'center' }}>
              <StatusPill status={st} />
            </span>
            <span className="small">{validityLabel(pkg, now)}</span>
          </div>
        </div>

        {conn !== 'open' && <div className="notice warn">Нет связи с сервером Selfvoy — переподключаемся…</div>}
        {notice && <div className="notice ok">{notice}</div>}
        {error && <div className="notice error">{error}</div>}
        {blocked && pkg.status !== 'pending' && <div className={`notice ${pkg.status === 'used' ? '' : 'warn'}`}>{blocked}</div>}

        {pkg.payment && <PaymentBlock payment={pkg.payment} note={pkg.payment_note} onClaim={claim} claiming={claiming} checking={checking} />}
        {query.paid && !checking && pkg.payment?.status === 'paid' && <div className="notice ok">Оплата получена — спасибо! Абонемент действует.</div>}

        <div className="group">
          <div className="group-head">{bookings.length ? `Записи по абонементу${upcoming ? ` · предстоит ${upcoming}` : ''}` : 'Записи по абонементу'}</div>
          <div className="group-body">
            {bookings.length === 0 && (
              <div className="cell">
                <span className="lab muted">Пока ни одной — выберите удобное время кнопкой ниже.</span>
              </div>
            )}
            {bookings.map((b) => {
              const d = new Date(b.start);
              const view = bookingView(b, now);
              return (
                <div key={`${b.start}-${b.status}`} className={`cell occ ${b.status === 'cancelled' ? 'cancelled' : ''}`}>
                  <span className="when">
                    {dayTitleIn(d, zone, now)}, {timeIn(d, zone)}
                  </span>
                  <StatusPill status={view} />
                </div>
              );
            })}
          </div>
          {bookings.length > 0 && <div className="group-foot">{timeZoneLabel(zone)}</div>}
        </div>

        <div className="group-body">
          {pkg.guest_name && (
            <div className="cell">
              <span className="lab">На имя</span>
              <span className="val">{pkg.guest_name}</span>
            </div>
          )}
          {pkg.cc_email && (
            <div className="cell">
              <span className="lab">
                Копия напоминаний<small>сюда приходят те же письма</small>
              </span>
              <span className="val ellipsis" style={{ maxWidth: '55%' }}>
                {pkg.cc_email}
              </span>
            </div>
          )}
          {pkg.price > 0 && (
            <div className="cell">
              <span className="lab">Стоимость</span>
              <span className="val num">{formatMoney(pkg.price, pkg.currency)}</span>
            </div>
          )}
          <div className="cell">
            <span className="lab">Специалист</span>
            <span className="val">{owner}</span>
          </div>
        </div>
      </GuestShell>
    );
  }

  return screens(loadError, () => void load(), 'Открываем абонемент…');
}
