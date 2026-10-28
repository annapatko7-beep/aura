/*
  Страница записи глазами гостя (#/b/<slug>) и ссылка управления записью (#/m/<токен>).

  Обе открываются без входа (и со входом тоже) вне оболочки приложения: booking.get /
  book / manage / reschedule / cancel / confirm / paymentClaim — публичные методы.
  Вид — «как приложение»: карточка услуги с ценой, календарь месяца со свободными
  днями, время пилюлями, липкая кнопка записи снизу, подпись «Запись через Selfvoy».

  После записи гость получает ссылку управления (сервер отдаёт токен один раз), блок
  оплаты, если у страницы предоплата (F4), и кнопку напоминаний в Telegram (F2).
  Ссылка управления понимает ?confirm=<start> — «Приду» из напоминания (F1) — и
  ?paid=1 — возврат со страницы оплаты ЮKassa (статус перечитывается booking.manage).
  Если готовую ссылку не собрать (нет SELFVOY_PUBLIC_URL, Electron/localhost), гостя
  сразу переводим на #/m/<токен> — именно её он сохранит в закладки. Файл .ics
  собирается здесь же, в браузере.

  F5 (пакеты): если у страницы есть предложения пакетов, гость выбирает «Разовое
  занятие» или пакет — booking.book с package_id; после записи — ссылка пакета
  #/p/<токен> (один раз) и оплата пакета. Со страницы пакета гость приходит сюда с
  ?pkg=<токен> — запись по абонементу (package_token), имя и email берутся из пакета.
  Необязательная «Копия напоминаний» — cc_email (≠ email гостя).
*/
import { CalendarPlus, Check, Copy, ExternalLink, Package, RefreshCw, WifiOff } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { Link, useLocation, useNavigate, useParams } from 'react-router-dom';
import { BrandingLink, GuestHead, GuestShell, MonthCalendar, PaymentBlock, TelegramBlock } from '../components/guest';
import { Field, Spinner, StatusPill } from '../components/ui';
import {
  bookingIcs,
  guestActions,
  isPlausibleSlug,
  isUpcoming,
  managePath,
  nextOccurrence,
  occurrenceAt,
  pagePath,
  parseManageQuery,
  publicLink,
  seriesLabel,
  sortedOccurrences,
  upcomingCount,
} from '../lib/booking';
import { genitive } from '../lib/format';
import {
  browserTimeZone,
  COMMENT_MAX,
  dayKey,
  EMAIL_MAX,
  GUEST_NAME_MAX,
  groupSlotsByDay,
  guestErrorKind,
  guestErrorText,
  guestFormErrors,
  hasErrors,
  icsUtc,
  isPlausibleToken,
  slotPhrase,
  timeIn,
  timeZoneLabel,
  type DayGroup,
  type GuestFormErrors,
} from '../lib/invite';
import { formatMoney } from '../lib/money';
import { shortDay } from '../lib/monthCalendar';
import { canBookByPackage, ccEmailError, leftLabel, packageBlockReason, packagePath, packageTokenFrom, perSessionLabel, sessionsLabel } from '../lib/packages';
import { policyLines, prepayLabel } from '../lib/payment';
import { makeBookingApi, type Booked, type GuestBooking, type GuestPage } from '../net/bookingApi';
import { makePackageApi, type GuestPackage, type PackageOffer } from '../net/packageApi';
import type { InviteSlot } from '../net/inviteApi';
import { useSession } from '../state/session';
import { copyText } from '../lib/clipboard';
import { GuestLayout, Message } from './InviteGuest';

const here = () => (typeof location === 'undefined' ? null : location);

type IcsBooking = Pick<GuestBooking, 'page_title' | 'owner_name' | 'occurrences' | 'reschedules_left'>;

/** icsKey — токен ссылки управления: из него UID событий, стабильный при переносе. */
function downloadBookingIcs(booking: IcsBooking, icsKey: string): boolean {
  try {
    const blob = new Blob([bookingIcs(booking, icsKey)], { type: 'text/calendar;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const first = sortedOccurrences(booking.occurrences).find((o) => o.status === 'active');
    const a = document.createElement('a');
    a.href = url;
    a.download = first ? `selfvoy-booking-${icsUtc(new Date(first.start)).slice(0, 8)}.ics` : 'selfvoy-booking.ics';
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

function CalendarButton({ booking, icsKey }: { booking: IcsBooking; icsKey: string }) {
  const [failed, setFailed] = useState(false);
  return (
    <>
      <button className="btn btn-secondary btn-lg btn-block" onClick={() => setFailed(!downloadBookingIcs(booking, icsKey))}>
        <CalendarPlus size={18} /> Добавить в календарь
      </button>
      {failed && <div className="field-error">Не удалось создать файл календаря.</div>}
    </>
  );
}

const DAYS_AT_ONCE = 4;

/** Выбор времени для переноса: слоты по дням в поясе гостя; сначала несколько ближайших дней. */
function SlotPicker({ groups, selected, onSelect }: { groups: DayGroup[]; selected: string | null; onSelect: (start: string) => void }) {
  const [days, setDays] = useState(DAYS_AT_ONCE);
  // Выбранный слот всегда на экране, даже если его день дальше показанных.
  const selectedDay = groups.findIndex((g) => g.slots.some((s) => s.start === selected));
  const shown = groups.slice(0, Math.max(days, selectedDay + 1));
  return (
    <div className="stack" style={{ gap: 14 }}>
      {shown.map((g) => (
        <div key={g.key} role="group" aria-label={g.title}>
          <div className="invite-day-title">{g.title}</div>
          <div className="invite-slots">
            {g.slots.map((s) => (
              <button key={s.start} type="button" className="invite-slot" aria-pressed={selected === s.start} onClick={() => onSelect(s.start)}>
                {s.label.split('–')[0]}
              </button>
            ))}
          </div>
        </div>
      ))}
      {shown.length < groups.length && (
        <button type="button" className="btn btn-ghost btn-sm" style={{ justifySelf: 'start' }} onClick={() => setDays((d) => d + DAYS_AT_ONCE)}>
          Показать ещё дни ({groups.length - shown.length})
        </button>
      )}
    </div>
  );
}

function ZoneHint({ zone, ownerZone, ownerName }: { zone: string; ownerZone: string; ownerName: string }) {
  const differs = ownerZone && timeIn(new Date(), ownerZone) !== timeIn(new Date(), zone);
  const ofOwner = ownerName ? genitive(ownerName) : 'организатора';
  return (
    <span className="hint" style={{ textAlign: 'center' }}>
      Время — в вашем часовом поясе: {timeZoneLabel(zone)}
      {differs ? `. У ${ofOwner} — ${timeZoneLabel(ownerZone)}` : ''}
    </span>
  );
}

/** Нет связи / ошибка загрузки / ожидание — общие экраны обеих страниц. */
export function guestScreens(conn: string, connError: unknown, retry: () => void) {
  return (loadError: string, reload: () => void, waiting: string): ReactNode => {
    if (loadError) {
      return (
        <Message
          title="Не удалось открыть страницу"
          action={
            <button className="btn btn-secondary" onClick={reload}>
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
          <div className="stack" style={{ textAlign: 'center', justifyItems: 'center' }}>
            <WifiOff size={22} className="faint" />
            <h1 className="title-lg">Нет связи с сервером Selfvoy</h1>
            <p className="muted" style={{ margin: 0 }}>
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
          <span className="muted small">{waiting}</span>
        </div>
      </div>
    );
  };
}

/** Ссылка управления записью: показывается один раз — сразу после записи. */
function ManageLink({ token, url }: { token: string; url: string }) {
  const link = publicLink(managePath(token), url, here());
  const [copied, setCopied] = useState(false);
  if (!link) {
    // Сюда обычно не попадаем: без готовой ссылки гостя сразу переводят на #/m/<токен>.
    return (
      <div className="notice">
        {/* Один дочерний элемент: .notice — flex, и текст с адресом иначе разъехался бы в три колонки. */}
        <span>
          Чтобы перенести или отменить встречу, откройте адрес <span className="mono break-all">#{managePath(token)}</span> этого сайта и сохраните его в закладки —
          или напишите организатору.
        </span>
      </div>
    );
  }
  return (
    <div className="group">
      <div className="group-head">Перенести или отменить</div>
      <div className="group-body">
        <div className="cell">
          <span className="lab">
            <span className="mono small ellipsis" style={{ display: 'block' }}>
              {link}
            </span>
            <small>Сохраните ссылку — она же в письме</small>
          </span>
          <button
            type="button"
            className="btn btn-secondary btn-sm"
            onClick={async () => {
              const ok = await copyText(link);
              setCopied(ok);
              if (ok) setTimeout(() => setCopied(false), 2000);
            }}
          >
            <Copy size={15} /> {copied ? 'Готово' : 'Копировать'}
          </button>
        </div>
      </div>
    </div>
  );
}

function OccurrenceList({ booking, zone }: { booking: Pick<GuestBooking, 'occurrences'>; zone: string }) {
  const list = sortedOccurrences(booking.occurrences);
  return (
    <div className="group-body">
      {list.map((o) => (
        <div key={o.start} className={`cell occ ${o.status === 'active' ? '' : 'cancelled'}`}>
          <span className="when">{slotPhrase(o, zone)}</span>
          {o.status !== 'active' ? (
            <StatusPill status={{ label: 'Отменена', tone: 'red', icon: 'x' }} />
          ) : o.confirmed_at ? (
            <StatusPill status={{ label: 'Вы придёте', tone: 'green', icon: 'check' }} />
          ) : null}
        </div>
      ))}
    </div>
  );
}

/** Карточка услуги: название, длительность, описание, цена. */
function ServiceCard({ title, minutes, description, price, currency }: { title: string; minutes: number; description?: string; price?: number; currency?: string }) {
  return (
    <div className="card svc">
      <div style={{ minWidth: 0 }}>
        <b>{title || 'Встреча'}</b>
        <small>
          {minutes} мин{description ? ` · ${description.split('\n')[0]}` : ''}
        </small>
      </div>
      {price ? <span className="pr">{formatMoney(price, currency)}</span> : null}
    </div>
  );
}

/** Условия: предоплата и политика отмены. С пакетом предоплата страницы не применяется. */
function Terms({ page, count, packaged }: { page: GuestPage; count: number; packaged?: boolean }) {
  const prepay = packaged ? '' : prepayLabel(page, count);
  const lines = policyLines(page.policy, packaged ? 'none' : page.prepay);
  // По пакету поздняя отмена не возвращает занятие (F5).
  if (packaged && page.policy && page.policy.cancel_hours > 0 && page.policy.late_cancel !== 'block' && lines.length > 1) lines[1] = 'Позже — занятие спишется с абонемента';
  if (!prepay && lines.length === 0) return null;
  return (
    <div className="group">
      <div className="group-head">Условия</div>
      <div className="group-body">
        {prepay && (
          <div className="cell">
            <span className="lab">
              {prepay}
              <small>при записи{page.pay_within_minutes ? `, оплатить в течение ${page.pay_within_minutes} мин` : ''}</small>
            </span>
          </div>
        )}
        {lines.length > 0 && (
          <div className="cell stacked">
            <ul className="policy" style={{ margin: 0, padding: 0 }}>
              {lines.map((l) => (
                <li key={l}>{l}</li>
              ))}
            </ul>
          </div>
        )}
      </div>
    </div>
  );
}

/** Выгода пакета относительно разовых занятий: «−25%» или "" (нет цены занятия или выгоды нет). */
function savings(offer: PackageOffer, single: number | undefined): string {
  if (!single || offer.sessions < 2) return '';
  const full = single * offer.sessions;
  const pct = Math.floor(((full - offer.price) / full) * 100);
  return pct >= 3 ? `−${pct}%` : '';
}

/** «Разовое занятие» или пакет занятий (F5): радиогруппа карточек. 0 — разовое. */
function PlanChoice({ page, value, onChange }: { page: GuestPage; value: number; onChange: (offerId: number) => void }) {
  const offers = (page.packages ?? []).filter((o) => typeof o.id === 'number');
  const currency = page.currency || 'RUB';
  return (
    <div className="group">
      <div className="group-head">Как записаться</div>
      <div className="plan-list" role="radiogroup" aria-label="Как записаться">
        <button type="button" role="radio" aria-checked={value === 0} className="plan-opt" onClick={() => onChange(0)}>
          <span className="radio" aria-hidden="true" />
          <span className="lab">
            <b>Разовое занятие</b>
            <small>{page.duration_minutes} мин</small>
          </span>
          {page.price ? <span className="pr num">{formatMoney(page.price, currency)}</span> : null}
        </button>
        {offers.map((o) => {
          const off = savings(o, page.price);
          return (
            <button key={o.id} type="button" role="radio" aria-checked={value === o.id} className="plan-opt" onClick={() => onChange(o.id as number)}>
              <span className="radio" aria-hidden="true" />
              <span className="lab">
                <b>{o.title}</b>
                <small>
                  {sessionsLabel(o.sessions)} · {perSessionLabel(o.price, o.sessions, currency)}
                  {o.valid_days ? ` · ${o.valid_days} дн.` : ''}
                </small>
              </span>
              <span className="pr num">
                {formatMoney(o.price, currency)}
                {off && <span className="badge green">{off}</span>}
              </span>
            </button>
          );
        })}
      </div>
    </div>
  );
}

/** Карточка «Запись по абонементу» над календарём (?pkg=<токен>). */
function PackageBanner({ pkg }: { pkg: GuestPackage }) {
  const blocked = packageBlockReason(pkg);
  return (
    <div className="card pkg-banner">
      <span className="ico amber" aria-hidden="true">
        <Package size={18} />
      </span>
      <div style={{ minWidth: 0, flex: 1 }}>
        <b>Запись по абонементу</b>
        <small>
          {pkg.title} · {leftLabel(pkg)}
          {pkg.guest_name ? ` · на имя ${pkg.guest_name}` : ''}
        </small>
        {blocked && <small className="field-error">{blocked}</small>}
      </div>
    </div>
  );
}

// ================================================================ #/b/<slug>

export function BookingGuest() {
  const { slug } = useParams();
  const navigate = useNavigate();
  const location = useLocation();
  const { socket, conn, connError, retry } = useSession();
  const api = useMemo(() => makeBookingApi(socket), [socket]);
  const packages = useMemo(() => makePackageApi(socket), [socket]);
  // Запись по абонементу: пришли со страницы пакета (#/b/<slug>?pkg=<токен>).
  const pkgToken = useMemo(() => packageTokenFrom(location.search), [location.search]);
  const [byPkg, setByPkg] = useState<GuestPackage | null>(null);
  const [pkgError, setPkgError] = useState('');
  const zone = useMemo(browserTimeZone, []);
  const screens = guestScreens(conn, connError, retry);

  const [page, setPage] = useState<GuestPage | null>(null);
  const [missing, setMissing] = useState(!isPlausibleSlug(slug));
  const [loadError, setLoadError] = useState('');
  const [done, setDone] = useState<Booked | null>(null);
  const doneRef = useRef(done);
  doneRef.current = done;
  const seq = useRef(0);
  const connRef = useRef(conn);
  connRef.current = conn;

  const [day, setDay] = useState<string | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [count, setCount] = useState(1);
  const [name, setName] = useState('');
  const [email, setEmail] = useState('');
  const [comment, setComment] = useState('');
  const [cc, setCc] = useState('');
  const [plan, setPlan] = useState(0); // 0 — разовое занятие, иначе id предложения пакета
  const [errors, setErrors] = useState<GuestFormErrors & { form?: string; cc?: string }>({});
  const [busy, setBusy] = useState(false);
  const nameRef = useRef<HTMLInputElement>(null);

  const apply = useCallback((next: GuestPage) => {
    setPage(next);
    setSelected((cur) => (cur && (next.slots ?? []).some((s) => s.start === cur) ? cur : null));
    setCount((c) => Math.min(c, Math.max(1, next.max_series)));
  }, []);

  const load = useCallback(async () => {
    if (!isPlausibleSlug(slug)) return;
    const mine = ++seq.current;
    try {
      const reply = await api.get(slug);
      if (mine !== seq.current || doneRef.current) return;
      setLoadError('');
      apply(reply.page);
    } catch (error) {
      if (mine !== seq.current) return;
      const kind = guestErrorKind(error);
      if (kind === 'notFound') setMissing(true);
      else if (kind !== 'network' || connRef.current === 'open') setLoadError(guestErrorText(error));
    }
  }, [api, slug, apply]);

  useEffect(() => {
    if (conn === 'open' && !doneRef.current) void load();
  }, [conn, load]);

  useEffect(() => {
    if (!pkgToken || conn !== 'open' || doneRef.current) return;
    packages
      .get(pkgToken)
      .then((r) => {
        setByPkg(r.package);
        setPkgError('');
      })
      .catch((e) => {
        if (guestErrorKind(e) === 'notFound') setPkgError('Ссылка абонемента не найдена — можно записаться как обычно.');
        else if (guestErrorKind(e) !== 'network') setPkgError(guestErrorText(e));
      });
  }, [pkgToken, conn, packages]);

  // Пакет этой страницы и по нему можно записаться — иначе обычная запись.
  const viaPkg = Boolean(pkgToken && byPkg && canBookByPackage(byPkg) && (!page || !byPkg.page_slug || byPkg.page_slug === page.slug));
  const offer = !viaPkg && plan ? (page?.packages ?? []).find((o) => o.id === plan) ?? null : null;
  const maxCount = Math.max(1, Math.min(page?.max_series ?? 1, viaPkg && byPkg ? byPkg.sessions_left : offer ? offer.sessions : Number.POSITIVE_INFINITY));
  const shownCount = Math.min(count, maxCount);

  const groups = useMemo(() => (page ? groupSlotsByDay(page.slots ?? [], zone) : []), [page, zone]);
  const freeDays = useMemo(() => groups.map((g) => g.key), [groups]);
  // Выбранный день пропал (время заняли) — первый свободный.
  const activeDay = day && freeDays.includes(day) ? day : (freeDays[0] ?? null);
  const daySlots = groups.find((g) => g.key === activeDay)?.slots ?? [];
  const selectedSlot = groups.flatMap((g) => g.slots).find((s) => s.start === selected) ?? null;

  const book = async () => {
    if (!page || !slug) return;
    const next: GuestFormErrors & { cc?: string } = guestFormErrors({ name, email, comment, slot: selected }, 'accept');
    // По абонементу имя и email берутся из пакета — поля можно не заполнять.
    if (viaPkg && !name.trim()) delete next.name;
    const ccError = viaPkg ? null : ccEmailError(cc, email);
    if (ccError) next.cc = ccError;
    setErrors(next);
    if (hasErrors(next) || !selected) {
      if (next.name) nameRef.current?.focus();
      return;
    }
    setBusy(true);
    try {
      const reply = await api.book(slug, {
        start: selected,
        guest_name: name.trim() || undefined,
        guest_email: email.trim() || undefined,
        cc_email: viaPkg ? undefined : cc.trim() || undefined,
        comment: comment.trim() || undefined,
        count: shownCount,
        package_id: offer?.id,
        package_token: viaPkg && pkgToken ? pkgToken : undefined,
      });
      seq.current += 1;
      // Купили пакет — ссылку пакета показываем здесь же (она одна на экране «Вы записаны»).
      if (!reply.package_token && !publicLink(managePath(reply.token), reply.url, here())) {
        // Ссылку управления не собрать — переходим на страницу записи (#/m/<токен>) вместо
        // #/b/<slug>: «сохранить в закладки» тогда сохраняет именно её, токен не теряется.
        navigate(managePath(reply.token), { replace: true, state: { booked: true, telegram_url: reply.telegram_url ?? '', package: pkgToken ?? '' } });
        return;
      }
      setDone(reply);
      window.scrollTo?.(0, 0);
    } catch (error) {
      const kind = guestErrorKind(error);
      if (kind === 'notFound') {
        setMissing(true);
      } else if (kind === 'conflict') {
        // Время заняли, серия упёрлась в занятую неделю или запись закрыли — показываем свежее состояние.
        try {
          const fresh = await api.get(slug);
          seq.current += 1;
          apply(fresh.page);
        } catch {
          /* оставляем как есть — текст ошибки ниже */
        }
        setErrors({ form: guestErrorText(error) });
      } else {
        setErrors({ form: guestErrorText(error) });
      }
    } finally {
      setBusy(false);
    }
  };

  if (missing) {
    return <Message title="Страница записи не найдена">Проверьте ссылку: возможно, она скопирована не полностью или запись по ней закрыта.</Message>;
  }

  if (done) {
    return (
      <BookedView
        booked={done}
        zone={zone}
        api={api}
        packageApi={packages}
        backToPackage={viaPkg ? pkgToken : null}
        onBooking={(booking) => setDone((d) => (d ? { ...d, booking } : d))}
        onPackage={(pkg) => setDone((d) => (d ? { ...d, package: pkg } : d))}
      />
    );
  }

  if (page) {
    const owner = page.owner_name || 'Организатор';
    if (page.status !== 'active') {
      // Новые записи закрыты, но уже записавшийся гость может запросить ссылку управления
      // (booking.resendLink отказывает только для удалённых страниц) — и отменить встречу.
      return (
        <GuestShell footer={<BrandingLink branding={page.branding} refCode={page.ref} />}>
          <GuestHead owner={page.owner_name} subtitle="Онлайн-запись" />
          <div className="card stack" style={{ textAlign: 'center' }}>
            <h1 className="title-md">Запись временно закрыта</h1>
            <p className="muted" style={{ margin: 0 }}>{`${owner} пока не принимает новые записи. Загляните позже.`}</p>
          </div>
          {slug && <LostLink api={api} slug={slug} />}
        </GuestShell>
      );
    }
    const seriesOptions = Array.from({ length: maxCount }, (_, i) => i + 1);
    const packaged = viaPkg || Boolean(offer);
    const prepay = packaged ? '' : prepayLabel(page, shownCount);
    const ctaHint = viaPkg && byPkg ? `Спишется ${sessionsLabel(shownCount)} из ${byPkg.sessions_left}` : offer ? `Пакет ${formatMoney(offer.price, page.currency)} · оплата после записи` : prepay ? `${prepay} · после записи` : '';
    const today = dayKey(new Date(), zone);
    const slotTitle = selectedSlot ? `${dayKey(new Date(selectedSlot.start), zone) === today ? 'сегодня' : shortDay(dayKey(new Date(selectedSlot.start), zone))}, ${timeIn(new Date(selectedSlot.start), zone)}` : '';
    const hasSlots = !page.slots_unavailable && groups.length > 0;
    const cta = hasSlots ? (
      <>
        <button className="btn btn-primary btn-lg btn-block" type="submit" form="guest-book" disabled={busy || !selectedSlot}>
          {busy ? <Spinner small /> : selectedSlot ? `Записаться на ${slotTitle}` : 'Выберите время'}
        </button>
        {ctaHint ? <small>{ctaHint}</small> : <BrandingLink branding={page.branding} refCode={page.ref} compact />}
      </>
    ) : undefined;

    return (
      <GuestShell cta={cta} footer={ctaHint || !hasSlots ? <BrandingLink branding={page.branding} refCode={page.ref} /> : undefined}>
        <GuestHead owner={page.owner_name} subtitle={viaPkg ? 'Запись по абонементу' : 'Онлайн-запись'} />
        <ServiceCard title={page.title} minutes={page.duration_minutes} description={page.description} price={viaPkg ? undefined : page.price} currency={page.currency} />
        {page.description && page.description.includes('\n') && <div className="notice invite-note">{page.description}</div>}
        {pkgToken && byPkg && <PackageBanner pkg={byPkg} />}
        {pkgToken && byPkg && byPkg.page_slug && byPkg.page_slug !== page.slug && <div className="notice warn">Этот абонемент — для другой страницы записи.</div>}
        {pkgError && <div className="notice warn">{pkgError}</div>}
        {!viaPkg && (page.packages ?? []).length > 0 && (
          <PlanChoice
            page={page}
            value={offer ? plan : 0}
            onChange={(id) => {
              setPlan(id);
              setErrors((e) => ({ ...e, form: undefined }));
            }}
          />
        )}
        <Terms page={page} count={shownCount} packaged={packaged} />
        {conn !== 'open' && <div className="notice warn">Нет связи с сервером Selfvoy — переподключаемся…</div>}

        {page.slots_unavailable ? (
          <div className="stack">
            <div className="notice warn">Не удалось загрузить свободное время. Попробуйте ещё раз через минуту.</div>
            <button className="btn btn-secondary" onClick={() => void load()}>
              <RefreshCw size={15} /> Обновить
            </button>
          </div>
        ) : groups.length === 0 ? (
          <div className="notice">Свободного времени сейчас нет — загляните позже или напишите {page.owner_name ? `организатору (${page.owner_name})` : 'организатору'} напрямую.</div>
        ) : (
          <form
            id="guest-book"
            className="stack"
            noValidate
            onSubmit={(e) => {
              e.preventDefault();
              void book();
            }}
          >
            <MonthCalendar
              freeDays={freeDays}
              selected={activeDay}
              todayKey={today}
              onSelect={(key) => {
                setDay(key);
                setSelected(null);
              }}
            />
            <div className="slots" role="group" aria-label="Свободное время">
              {daySlots.map((s) => (
                <button
                  key={s.start}
                  type="button"
                  aria-pressed={selected === s.start}
                  onClick={() => {
                    setSelected(s.start);
                    setErrors((e) => ({ ...e, slot: undefined, form: undefined }));
                  }}
                >
                  {timeIn(new Date(s.start), zone)}
                </button>
              ))}
            </div>
            {errors.slot ? <span className="field-error">{errors.slot}</span> : <ZoneHint zone={zone} ownerZone={page.owner_timezone} ownerName={page.owner_name} />}

            {selectedSlot && (
              <div className="group">
                <div className="group-head">Ваши данные</div>
                <div className="group-body">
                  <div className="cell stacked">
                    {maxCount > 1 && (
                      <Field label="Сколько встреч" hint="Та же встреча каждую неделю в это же время">
                        <select className="input" value={shownCount} onChange={(e) => setCount(Number(e.target.value))}>
                          {seriesOptions.map((n) => (
                            <option key={n} value={n}>
                              {seriesLabel(n)}
                            </option>
                          ))}
                        </select>
                      </Field>
                    )}
                    <Field label="Ваше имя" error={errors.name} hint={viaPkg && byPkg?.guest_name ? `Необязательно — запишем на имя ${byPkg.guest_name}` : undefined}>
                      <input ref={nameRef} className={`input ${errors.name ? 'invalid' : ''}`} autoComplete="name" value={name} maxLength={GUEST_NAME_MAX} onChange={(e) => setName(e.target.value)} />
                    </Field>
                    <Field
                      label="Email"
                      error={errors.email}
                      hint={viaPkg ? 'Необязательно — возьмём из абонемента' : offer || (page.prepay && page.prepay !== 'none') ? 'Пришлём подтверждение, ссылку оплаты и напоминания' : 'Необязательно — для подтверждения и напоминаний'}
                    >
                      <input className={`input ${errors.email ? 'invalid' : ''}`} type="email" inputMode="email" autoComplete="email" placeholder="you@example.com" value={email} maxLength={EMAIL_MAX} onChange={(e) => setEmail(e.target.value)} />
                    </Field>
                    {!viaPkg && (
                      <Field label="Копия напоминаний (например, родителю)" error={errors.cc} hint="Необязательно — сюда придут те же письма: подтверждение, напоминания, перенос">
                        <input
                          className={`input ${errors.cc ? 'invalid' : ''}`}
                          type="email"
                          inputMode="email"
                          autoComplete="off"
                          placeholder="parent@example.com"
                          value={cc}
                          maxLength={EMAIL_MAX}
                          onChange={(e) => {
                            setCc(e.target.value);
                            setErrors((x) => (x.cc ? { ...x, cc: undefined } : x));
                          }}
                        />
                      </Field>
                    )}
                    <Field label="Комментарий" error={errors.comment} hint="Необязательно">
                      <textarea className="textarea" style={{ minHeight: 64 }} value={comment} maxLength={COMMENT_MAX} onChange={(e) => setComment(e.target.value)} />
                    </Field>
                  </div>
                </div>
              </div>
            )}
            {errors.form && (
              <div className="notice error" role="alert">
                {errors.form}
              </div>
            )}
          </form>
        )}
        {slug && <LostLink api={api} slug={slug} />}
      </GuestShell>
    );
  }

  return screens(loadError, () => void load(), 'Открываем страницу записи…');
}

/** Ссылка пакета (F5): показывается один раз — сразу после покупки. */
function PackageLink({ token, url, pkg }: { token: string; url: string; pkg?: GuestPackage }) {
  const link = publicLink(packagePath(token), url, here());
  const [copied, setCopied] = useState(false);
  return (
    <div className="group">
      <div className="group-head">Ваш абонемент</div>
      <div className="group-body">
        <div className="cell has-ico">
          <span className="ico amber">
            <Package size={17} />
          </span>
          <span className="lab">
            {pkg?.title || 'Пакет занятий'}
            <small>{pkg ? `${leftLabel(pkg)} · по ссылке — остаток и запись` : 'По ссылке — остаток занятий и запись'}</small>
          </span>
        </div>
        <div className="cell">
          <span className="lab">
            <span className="mono small ellipsis" style={{ display: 'block' }}>
              {link ?? `#${packagePath(token)}`}
            </span>
            <small>Сохраните ссылку — она же в письме</small>
          </span>
          <span className="row" style={{ gap: 6 }}>
            <button
              type="button"
              className="btn btn-secondary btn-sm"
              onClick={async () => {
                const ok = await copyText(link ?? `${location.origin}${location.pathname}#${packagePath(token)}`);
                setCopied(ok);
                if (ok) setTimeout(() => setCopied(false), 2000);
              }}
            >
              <Copy size={15} /> {copied ? 'Готово' : 'Копировать'}
            </button>
            <Link className="icon-btn" to={packagePath(token)} aria-label="Открыть абонемент">
              <ExternalLink size={16} />
            </Link>
          </span>
        </div>
      </div>
    </div>
  );
}

/** «Вы записаны»: встречи, оплата (если нужна), пакет (F5), Telegram, календарь, ссылка управления. */
function BookedView({
  booked,
  zone,
  api,
  packageApi,
  backToPackage,
  onBooking,
  onPackage,
}: {
  booked: Booked;
  zone: string;
  api: ReturnType<typeof makeBookingApi>;
  packageApi: ReturnType<typeof makePackageApi>;
  /** Записались по абонементу — токен его ссылки (вернуться к остатку). */
  backToPackage: string | null;
  onBooking: (booking: GuestBooking) => void;
  onPackage: (pkg: GuestPackage) => void;
}) {
  const booking = booked.booking;
  const owner = booking.owner_name;
  const pkg = booked.package;
  // Оплата пакета живёт у пакета; у записи её дубль (kind: package) не показываем.
  const pkgPayment = booked.package_token ? (pkg?.payment ?? null) : null;
  const payment = pkgPayment || booking.payment?.kind === 'package' ? null : booking.payment;
  const [claiming, setClaiming] = useState(false);
  const [error, setError] = useState('');
  const mustPay = payment?.status === 'pending' || pkgPayment?.status === 'pending';
  const claim = async () => {
    setClaiming(true);
    setError('');
    try {
      onBooking((await api.paymentClaim(booked.token)).booking);
    } catch (e) {
      setError(guestErrorText(e));
    } finally {
      setClaiming(false);
    }
  };
  const claimPackage = async () => {
    if (!booked.package_token) return;
    setClaiming(true);
    setError('');
    try {
      onPackage((await packageApi.paymentClaim(booked.package_token)).package);
    } catch (e) {
      setError(guestErrorText(e));
    } finally {
      setClaiming(false);
    }
  };
  return (
    <GuestShell footer={<BrandingLink branding={booking.branding} refCode={booking.ref} />}>
      <div className="g-done">
        <span className="invite-done" aria-hidden="true">
          <Check size={28} strokeWidth={2.6} />
        </span>
        <h1 className="title-lg">{mustPay ? 'Осталось оплатить' : 'Вы записаны'}</h1>
        <p className="muted" style={{ margin: 0 }}>
          {mustPay ? 'Время за вами — оплатите, чтобы запись осталась в силе.' : owner ? `${owner} получил(а) уведомление.` : 'Организатор получил уведомление.'}
        </p>
      </div>
      <div className="group">
        <div className="group-head">{booking.page_title || 'Встреча'}</div>
        <OccurrenceList booking={booking} zone={zone} />
        <div className="group-foot">{timeZoneLabel(zone)}</div>
      </div>
      {pkgPayment && <PaymentBlock payment={pkgPayment} note={pkg?.payment_note || booking.payment_note} onClaim={claimPackage} claiming={claiming} />}
      {payment && <PaymentBlock payment={payment} note={booking.payment_note} onClaim={claim} claiming={claiming} />}
      {error && <div className="notice error">{error}</div>}
      {booked.package_token && <PackageLink token={booked.package_token} url={booked.package_url ?? ''} pkg={pkg} />}
      {!booked.package_token && pkg && (
        <div className="notice ok">
          <Package size={16} />
          <span>
            Списано с абонемента «{pkg.title}»: {leftLabel(pkg)}.{' '}
            {backToPackage && (
              <Link className="link" to={packagePath(backToPackage)}>
                К абонементу
              </Link>
            )}
          </span>
        </div>
      )}
      <TelegramBlock url={booked.telegram_url || booking.telegram_url} linked={booking.telegram_linked} />
      <CalendarButton booking={booking} icsKey={booked.token} />
      <ManageLink token={booked.token} url={booked.url} />
    </GuestShell>
  );
}

/** «Уже записались?» — новая ссылка управления на email, указанный при записи. */
function LostLink({ api, slug }: { api: ReturnType<typeof makeBookingApi>; slug: string }) {
  const [open, setOpen] = useState(false);
  const [email, setEmail] = useState('');
  const [state, setState] = useState<'idle' | 'busy' | 'sent'>('idle');
  const [error, setError] = useState('');
  if (!open) {
    return (
      <button type="button" className="btn btn-ghost btn-sm" style={{ justifySelf: 'center' }} onClick={() => setOpen(true)}>
        Уже записались? Пришлём ссылку для переноса или отмены
      </button>
    );
  }
  if (state === 'sent') {
    return <div className="notice">Если вы записывались с этим email, новая ссылка уже в письме. Прежняя ссылка больше не действует.</div>;
  }
  return (
    <form
      className="card stack"
      noValidate
      onSubmit={async (e) => {
        e.preventDefault();
        const value = email.trim();
        if (!/^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(value)) {
          setError('Похоже, в email опечатка');
          return;
        }
        setState('busy');
        setError('');
        try {
          await api.resendLink(slug, value);
          setState('sent');
        } catch (err) {
          setState('idle');
          setError(guestErrorText(err));
        }
      }}
    >
      <Field label="Email, указанный при записи" error={error}>
        <input className={`input ${error ? 'invalid' : ''}`} type="email" inputMode="email" autoComplete="email" value={email} maxLength={EMAIL_MAX} onChange={(e) => setEmail(e.target.value)} />
      </Field>
      <button className="btn btn-secondary" type="submit" disabled={state === 'busy'}>
        {state === 'busy' ? <Spinner small /> : 'Прислать ссылку'}
      </button>
    </form>
  );
}

// ================================================================ #/m/<токен>

/** Сколько раз перечитать запись после возврата с оплаты (ЮKassa отвечает не мгновенно). */
const PAID_POLLS = 4;
const PAID_POLL_MS = 4000;

export function BookingManage() {
  const { token } = useParams();
  const location = useLocation();
  const navigate = useNavigate();
  const { socket, conn, connError, retry } = useSession();
  const api = useMemo(() => makeBookingApi(socket), [socket]);
  const zone = useMemo(browserTimeZone, []);
  const screens = guestScreens(conn, connError, retry);
  const query = useMemo(() => parseManageQuery(location.search), [location.search]);
  const navState = location.state as { booked?: boolean; telegram_url?: string; package?: string } | null;

  const [booking, setBooking] = useState<GuestBooking | null>(null);
  const [missing, setMissing] = useState(!isPlausibleToken(token));
  const [loadError, setLoadError] = useState('');
  const [mode, setMode] = useState<'view' | 'move' | 'cancel'>('view');
  const [selected, setSelected] = useState<string | null>(null);
  const [cancelTarget, setCancelTarget] = useState<string | null>(null); // start одной встречи или null — все
  const [reason, setReason] = useState('');
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [busy, setBusy] = useState<'' | 'act' | 'confirm' | 'claim'>('');
  const [thanks, setThanks] = useState(false);
  const [checking, setChecking] = useState(query.paid);
  // Пришли сюда сразу после записи (готовой ссылки управления не было) — эту страницу и сохраняют.
  const justBooked = Boolean(navState?.booked);
  const seq = useRef(0);
  const connRef = useRef(conn);
  connRef.current = conn;
  const confirmTried = useRef(false);
  const polls = useRef(0);

  const load = useCallback(async () => {
    if (!isPlausibleToken(token)) return null;
    const mine = ++seq.current;
    try {
      const reply = await api.manage(token);
      if (mine !== seq.current) return null;
      setLoadError('');
      setBooking(reply.booking);
      setSelected((cur) => (cur && (reply.booking.slots ?? []).some((s) => s.start === cur) ? cur : null));
      return reply.booking;
    } catch (e) {
      if (mine !== seq.current) return null;
      const kind = guestErrorKind(e);
      if (kind === 'notFound') setMissing(true);
      else if (kind !== 'network' || connRef.current === 'open') setLoadError(guestErrorText(e));
      return null;
    }
  }, [api, token]);

  const confirm = useCallback(
    async (occurrence?: string) => {
      if (!token) return;
      setBusy('confirm');
      setError('');
      try {
        const reply = await api.confirm(token, occurrence);
        seq.current += 1;
        setBooking(reply.booking);
        setThanks(true);
        setNotice('');
      } catch (e) {
        if (guestErrorKind(e) === 'notFound') setMissing(true);
        else setError(guestErrorKind(e) === 'conflict' ? 'Эта встреча уже прошла или отменена — подтверждать нечего.' : guestErrorText(e));
      } finally {
        setBusy('');
      }
    },
    [api, token],
  );

  useEffect(() => {
    if (conn !== 'open') return;
    void (async () => {
      const fresh = await load();
      // «Приду» из напоминания: один раз за открытие ссылки, повтор на сервере ничего не меняет.
      if (fresh && query.confirm && !confirmTried.current) {
        confirmTried.current = true;
        // Время из письма — в той записи, что прислал сервер (иначе сервер выберет ближайшую сам).
        await confirm(occurrenceAt(fresh.occurrences, query.confirm)?.start ?? query.confirm);
        navigate({ pathname: location.pathname, search: query.paid ? '?paid=1' : '' }, { replace: true, state: navState });
      }
    })();
  }, [conn, load]); // eslint-disable-line react-hooks/exhaustive-deps

  // Вернулись со страницы оплаты (?paid=1): ЮKassa подтверждает не сразу — перечитываем несколько раз.
  useEffect(() => {
    if (!query.paid || conn !== 'open' || !booking) return;
    if (booking.payment?.status !== 'pending' || polls.current >= PAID_POLLS) {
      setChecking(false);
      return;
    }
    const timer = setTimeout(() => {
      polls.current += 1;
      void load();
    }, PAID_POLL_MS);
    return () => clearTimeout(timer);
  }, [query.paid, conn, booking, load]);

  const groups = useMemo(() => (booking ? groupSlotsByDay(booking.slots ?? [], zone) : []), [booking, zone]);

  const act = async (action: () => Promise<{ booking: GuestBooking }>, success: string) => {
    setBusy('act');
    setError('');
    try {
      const reply = await action();
      seq.current += 1;
      setBooking(reply.booking);
      setMode('view');
      setSelected(null);
      setReason('');
      setNotice(success);
      setThanks(false);
    } catch (e) {
      if (guestErrorKind(e) === 'notFound') setMissing(true);
      else setError(guestErrorText(e));
      if (guestErrorKind(e) === 'conflict') void load();
    } finally {
      setBusy('');
    }
  };

  const claim = async () => {
    if (!token) return;
    setBusy('claim');
    setError('');
    try {
      const reply = await api.paymentClaim(token);
      seq.current += 1;
      setBooking(reply.booking);
      setNotice('Спасибо! Специалист проверит оплату.');
    } catch (e) {
      setError(guestErrorText(e));
    } finally {
      setBusy('');
    }
  };

  if (missing) {
    return <Message title="Запись не найдена">Проверьте ссылку: возможно, она скопирована не полностью.</Message>;
  }

  if (booking) {
    const owner = booking.owner_name || 'Организатор';
    const next = nextOccurrence(booking.occurrences);
    const upcoming = upcomingCount(booking.occurrences);
    const { canCancel, canReschedule } = guestActions(booking);
    // Права могли смениться после перезагрузки (страницу поставили на паузу) — шаг пропадает.
    const step = (mode === 'move' && !canReschedule) || (mode === 'cancel' && !canCancel) ? 'view' : mode;
    const telegramUrl = booking.telegram_url || navState?.telegram_url || '';
    const paid = booking.payment?.status === 'paid' || booking.payment?.status === 'claimed';
    const policy = booking.policy;
    const lines = policyLines(policy, booking.payment ? 'full' : 'none');
    const footer = <BrandingLink branding={booking.branding} refCode={booking.ref} />;
    const againLink = booking.page_slug ? (
      <Link className="btn btn-primary btn-lg btn-block" to={pagePath(booking.page_slug)}>
        Выбрать другое время
      </Link>
    ) : null;

    if (booking.status === 'cancelled') {
      return (
        <GuestShell footer={footer}>
          <GuestHead owner={booking.owner_name} subtitle={booking.page_title} />
          <div className="card stack" style={{ textAlign: 'center' }}>
            <h1 className="title-md">Запись отменена</h1>
            <p className="muted" style={{ margin: 0 }}>
              {booking.cancelled_by === 'system'
                ? 'Оплата не поступила вовремя, поэтому время освободилось. Можно записаться снова.'
                : booking.cancelled_by === 'owner'
                  ? `${owner} отменил(а) встречу. Можно выбрать другое время.`
                  : `${owner} получил(а) уведомление об отмене.`}
            </p>
          </div>
          {notice && <div className="notice ok">{notice}</div>}
          {booking.by_package && (
            <div className="notice">
              <Package size={16} /> Запись была по абонементу{booking.cancelled_by === 'owner' ? ' — занятие вернулось на абонемент.' : '.'}
            </div>
          )}
          <OccurrenceList booking={booking} zone={zone} />
          {booking.payment && <PaymentBlock payment={booking.payment} />}
          {againLink}
        </GuestShell>
      );
    }

    // «Приду» — липкой кнопкой; пока ждём оплату, главная кнопка — оплата, «Приду» — в карточке встречи.
    const awaitingPay = booking.payment?.status === 'pending';
    const needConfirm = step === 'view' && next && !next.confirmed_at && !awaitingPay;
    const confirmedNext = next?.confirmed_at;
    const cta = needConfirm ? (
      <>
        <button type="button" className="btn btn-primary btn-lg btn-block" disabled={busy === 'confirm'} onClick={() => void confirm(next.start)}>
          {busy === 'confirm' ? <Spinner small /> : 'Приду'}
        </button>
        <small>{owner} увидит, что вы придёте</small>
      </>
    ) : undefined;

    return (
      <GuestShell cta={cta} footer={footer}>
        <GuestHead owner={booking.owner_name} subtitle={booking.page_title || 'Ваша запись'} />
        {thanks && (
          <div className="g-done">
            <span className="invite-done" aria-hidden="true">
              <Check size={28} strokeWidth={2.6} />
            </span>
            <h1 className="title-lg">Спасибо, ждём вас</h1>
            <p className="muted" style={{ margin: 0 }}>
              {owner} увидит, что вы придёте.
            </p>
          </div>
        )}
        {justBooked && !thanks && !notice && (
          <div className="notice ok">
            <Check size={16} />
            <span>
              Вы записаны — {booking.owner_name ? `${booking.owner_name} получил(а) уведомление` : 'организатор получил уведомление'}. Сохраните эту страницу в закладки: по ней можно
              перенести или отменить встречу.
            </span>
          </div>
        )}
        {conn !== 'open' && <div className="notice warn">Нет связи с сервером Selfvoy — переподключаемся…</div>}
        {notice && <div className="notice ok">{notice}</div>}
        {error && <div className="notice error">{error}</div>}

        {next && (
          <div className="card stack" style={{ gap: 4 }}>
            <span className="small muted">Ближайшая встреча</span>
            <span className="title-md">{slotPhrase(next, zone)}</span>
            <span className="row wrap" style={{ gap: 6, marginTop: 4 }}>
              {confirmedNext ? <StatusPill status={{ label: 'Вы подтвердили', tone: 'green', icon: 'check' }} /> : <StatusPill status={{ label: 'Ждём подтверждения', tone: 'new', icon: 'clock' }} />}
              {booking.by_package && <StatusPill status={{ label: 'по абонементу', tone: 'accent', icon: 'calendar' }} />}
              <span className="small faint">{timeZoneLabel(zone)}</span>
            </span>
            {!confirmedNext && awaitingPay && step === 'view' && (
              <button type="button" className="btn btn-secondary btn-sm" style={{ justifySelf: 'start', marginTop: 6 }} disabled={busy === 'confirm'} onClick={() => void confirm(next.start)}>
                {busy === 'confirm' ? <Spinner small /> : <Check size={15} />} Приду
              </button>
            )}
          </div>
        )}

        {booking.occurrences.length > 1 && (
          <div className="group">
            <div className="group-head">{seriesLabel(booking.occurrences.length)}</div>
            <OccurrenceList booking={booking} zone={zone} />
          </div>
        )}

        {upcoming === 0 && <div className="notice">Все встречи этой записи уже прошли.</div>}

        {booking.cc_email && (
          <div className="group-body">
            <div className="cell">
              <span className="lab">
                Копия напоминаний<small>сюда приходят те же письма</small>
              </span>
              <span className="val ellipsis" style={{ maxWidth: '55%' }}>
                {booking.cc_email}
              </span>
            </div>
          </div>
        )}
        {navState?.package && (
          <Link className="btn btn-secondary btn-lg btn-block" to={packagePath(navState.package)}>
            <Package size={18} /> К абонементу
          </Link>
        )}

        {booking.payment && (
          <PaymentBlock payment={booking.payment} note={booking.payment_note} onClaim={claim} claiming={busy === 'claim'} checking={checking} />
        )}
        {query.paid && !checking && booking.payment?.status === 'paid' && <div className="notice ok">Оплата получена — спасибо!</div>}

        {upcoming > 0 && <TelegramBlock url={telegramUrl} linked={booking.telegram_linked} />}

        {booking.late && policy && (
          <div className="notice warn">
            {policy.late_cancel === 'block'
              ? `До встречи меньше ${policy.cancel_hours} ч — перенести или отменить можно только через специалиста.`
              : paid
                ? `До встречи меньше ${policy.cancel_hours} ч — при отмене оплата не вернётся.`
                : `До встречи меньше ${policy.cancel_hours} ч — пожалуйста, предупредите специалиста.`}
          </div>
        )}

        {step === 'view' && upcoming > 0 && (
          <div className="stack">
            {token && <CalendarButton booking={booking} icsKey={token} />}
            {(canReschedule || canCancel) && (
              <div className="grid-2">
                {canReschedule && (
                  <button className="btn btn-secondary btn-lg" onClick={() => setMode('move')}>
                    Перенести
                  </button>
                )}
                {canCancel && (
                  <button
                    className="btn btn-danger btn-lg"
                    onClick={() => {
                      setCancelTarget(null);
                      setMode('cancel');
                    }}
                  >
                    Отменить
                  </button>
                )}
              </div>
            )}
            {canCancel && !canReschedule && !booking.late && (
              <span className="hint">
                {booking.reschedules_left <= 0 ? 'Перенести можно не больше трёх раз — дальше только отмена.' : `Перенос временно недоступен: ${owner} сейчас не принимает новые записи. Отменить встречу можно.`}
              </span>
            )}
          </div>
        )}

        {step === 'move' && next && (
          <div className="stack">
            <div className="small muted">
              Переносим встречу {slotPhrase(next, zone)}. Осталось переносов: {booking.reschedules_left}.
            </div>
            {booking.slots_unavailable ? (
              <div className="notice warn">Не удалось загрузить свободное время. Попробуйте ещё раз через минуту.</div>
            ) : groups.length === 0 ? (
              <div className="notice">Свободного времени для переноса сейчас нет.</div>
            ) : (
              <>
                <SlotPicker groups={groups} selected={selected} onSelect={setSelected} />
                <ZoneHint zone={zone} ownerZone={booking.owner_timezone} ownerName={booking.owner_name} />
              </>
            )}
            <div className="grid-2">
              <button className="btn btn-primary btn-lg" disabled={Boolean(busy) || !selected} onClick={() => token && selected && void act(() => api.reschedule(token, selected, next.start), 'Встреча перенесена.')}>
                {busy === 'act' ? <Spinner small /> : 'Перенести сюда'}
              </button>
              <button className="btn btn-ghost btn-lg" disabled={Boolean(busy)} onClick={() => setMode('view')}>
                Назад
              </button>
            </div>
          </div>
        )}

        {step === 'cancel' && (
          <div className="card stack">
            {upcoming > 1 && (
              <Field label="Что отменить">
                <select className="input" value={cancelTarget ?? ''} onChange={(e) => setCancelTarget(e.target.value || null)}>
                  <option value="">Все предстоящие встречи ({upcoming})</option>
                  {sortedOccurrences(booking.occurrences)
                    .filter((o) => isUpcoming(o))
                    .map((o) => (
                      <option key={o.start} value={o.start}>
                        Только {slotPhrase(o as InviteSlot, zone)}
                      </option>
                    ))}
                </select>
              </Field>
            )}
            <Field label="Причина" hint="Необязательно — её увидит организатор">
              <textarea className="textarea" style={{ minHeight: 56 }} value={reason} maxLength={COMMENT_MAX} onChange={(e) => setReason(e.target.value)} />
            </Field>
            {booking.late && paid && <span className="hint">Оплата за позднюю отмену не возвращается.</span>}
            {booking.late && booking.by_package && <span className="hint">Поздняя отмена — занятие спишется с абонемента.</span>}
            <div className="grid-2">
              <button
                className="btn btn-danger btn-lg"
                disabled={Boolean(busy)}
                onClick={() => token && void act(() => api.cancel(token, { occurrence: cancelTarget ?? undefined, reason: reason.trim() || undefined }), cancelTarget ? 'Встреча отменена.' : 'Запись отменена.')}
              >
                {busy === 'act' ? <Spinner small /> : 'Да, отменить'}
              </button>
              <button className="btn btn-ghost btn-lg" disabled={Boolean(busy)} onClick={() => setMode('view')}>
                Назад
              </button>
            </div>
          </div>
        )}

        {lines.length > 0 && (
          <div className="group">
            <div className="group-head">Условия</div>
            <div className="group-body">
              <div className="cell stacked">
                <ul className="policy" style={{ margin: 0, padding: 0 }}>
                  {lines.map((l) => (
                    <li key={l}>{l}</li>
                  ))}
                </ul>
              </div>
            </div>
          </div>
        )}
      </GuestShell>
    );
  }

  return screens(loadError, () => void load(), 'Открываем запись…');
}
