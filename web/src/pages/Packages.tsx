/*
  «Абонементы» — пакеты занятий клиентов (F5, package.*).

  Список с фильтром по статусу и странице: у каждого — сколько занятий использовано
  из скольких (полоса) и остаток. Карточка пакета: поправить остаток (±), отметить
  оплату (paid / waived / refunded), отменить. «Выдать абонемент» — продажа вне
  сайта (package.issue): готовое предложение страницы или своё; сервер возвращает
  ссылку пакета #/p/<токен> один раз — её можно скопировать или отправить клиенту.
  Список обновляется по уведомлениям package.* и booking.*.
*/
import { Ban, Check, Copy, Minus, Package, Plus, Share2 } from 'lucide-react';
import { useCallback, useEffect, useMemo, useRef, useState, type FormEvent } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { MoneyInput } from '../components/MoneyInput';
import { Avatar, Empty, Field, Group, Segmented, Sheet, Spinner, StatusPill, Switch } from '../components/ui';
import { SkeletonList } from '../components/Skeleton';
import { bookingOwnerErrorText, publicLink } from '../lib/booking';
import { canShare, copyText } from '../lib/clipboard';
import { plural, when } from '../lib/format';
import { hasErrors, EMAIL_MAX, GUEST_NAME_MAX } from '../lib/invite';
import { formatMoney } from '../lib/money';
import {
  issueErrors,
  leftLabel,
  OFFER_DEFAULT_DAYS,
  OFFER_TITLE_MAX,
  offerSummary,
  PACKAGE_FILTERS,
  packagePath,
  packageProgress,
  packageStatusView,
  sessionsLabel,
  validityLabel,
  type IssueErrors,
  type IssueForm,
} from '../lib/packages';
import { OWNER_PAYMENT_ACTION, ownerPaymentActions, paymentLabel } from '../lib/payment';
import { makeBookingApi, type BookingPage, type OwnerPaymentStatus } from '../net/bookingApi';
import { makePackageApi, type Issued, type OwnerPackage, type PackageApi, type PackageStatus } from '../net/packageApi';
import { isNetworkError } from '../net/socket';
import { useData } from '../state/data';
import { useSession } from '../state/session';

const here = () => (typeof location === 'undefined' ? null : location);

export function Packages() {
  const { socket, conn } = useSession();
  const { notify } = useData();
  const api = useMemo(() => makePackageApi(socket), [socket]);
  const booking = useMemo(() => makeBookingApi(socket), [socket]);
  const [params, setParams] = useSearchParams();
  const [status, setStatus] = useState<'' | PackageStatus>('');
  const [pageId, setPageId] = useState(0);
  const [list, setList] = useState<OwnerPackage[] | null>(null);
  const [pages, setPages] = useState<BookingPage[] | null>(null);
  const [error, setError] = useState('');
  const [openId, setOpenId] = useState<number | null>(null);
  const [issuing, setIssuing] = useState(false);
  const seq = useRef(0);

  const reload = useCallback(async () => {
    const mine = ++seq.current;
    try {
      const [p, l] = await Promise.all([booking.listPages(), api.list({ status: status || undefined, page_id: pageId || undefined, limit: 200 })]);
      if (mine !== seq.current) return;
      setPages(p.pages ?? []);
      setList(l.packages ?? []);
      setError('');
    } catch (e) {
      if (mine !== seq.current) return;
      if (!isNetworkError(e)) setError(bookingOwnerErrorText(e));
    }
  }, [api, booking, status, pageId]);

  useEffect(() => {
    if (conn === 'open') void reload();
  }, [conn, reload]);

  useEffect(
    () =>
      socket.onEvent((event) => {
        if (event.event !== 'notification.new') return;
        const kind = String((event.payload as { kind?: unknown }).kind ?? '');
        if (kind.startsWith('package.') || kind.startsWith('booking.')) void reload();
      }),
    [socket, reload],
  );

  useEffect(
    () => () => {
      seq.current += 1;
    },
    [],
  );

  // ?id=<пакет> — из уведомления или карточки записи; ?new=1 — сразу «Выдать абонемент».
  useEffect(() => {
    const id = Number(params.get('id') ?? 0);
    if (id > 0) {
      setOpenId(id);
      setStatus('');
      setParams({}, { replace: true });
    } else if (params.get('new') === '1') {
      setIssuing(true);
      setParams({}, { replace: true });
    }
  }, [params, setParams]);

  const put = (pkg: OwnerPackage) => setList((l) => (l ?? []).map((x) => (x.id === pkg.id ? pkg : x)));
  const opened = openId ? (list ?? []).find((p) => p.id === openId) ?? null : null;

  const totals = useMemo(() => {
    const all = list ?? [];
    return {
      active: all.filter((p) => p.status === 'active').length,
      pending: all.filter((p) => p.status === 'pending' || p.payment?.status === 'claimed').length,
      left: all.filter((p) => p.status === 'active').reduce((sum, p) => sum + Math.max(0, p.sessions_left), 0),
    };
  }, [list]);

  return (
    <div className="page">
      <header className="page-head">
        <div>
          <h1 className="title-xl">Абонементы</h1>
          <div className="sub">Пакеты занятий клиентов: остаток, срок и оплата.</div>
        </div>
        {pages !== null && (
          <button className="btn btn-primary" onClick={() => setIssuing(true)}>
            <Plus size={17} /> Выдать
          </button>
        )}
      </header>

      <div className="stack-lg">
        {error && <div className="notice error">{error}</div>}

        {list && list.length > 0 && !status && !pageId && (
          <div className="pkg-kpis">
            <div className="card">
              <b>{totals.active}</b>
              <small>{plural(totals.active, 'действует', 'действуют', 'действуют')}</small>
            </div>
            <div className="card">
              <b>{totals.pending}</b>
              <small>ждут оплаты</small>
            </div>
            <div className="card">
              <b>{totals.left}</b>
              <small>{plural(totals.left, 'занятие', 'занятия', 'занятий')} впереди</small>
            </div>
          </div>
        )}

        <section className="stack">
          <div className="sec-h" style={{ marginTop: 0, flexWrap: 'wrap', gap: 8 }}>
            <Segmented label="Какие абонементы показать" value={status} onChange={setStatus} options={PACKAGE_FILTERS} />
            {pages && pages.length > 1 && (
              <select className="input" style={{ width: 'auto', height: 34 }} aria-label="Страница записи" value={pageId} onChange={(e) => setPageId(Number(e.target.value))}>
                <option value={0}>Все страницы</option>
                {pages.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.title}
                  </option>
                ))}
              </select>
            )}
          </div>
          <div className="card flush list">
            {list === null && !error && (
              <SkeletonList rows={3} />
            )}
            {list?.length === 0 && (
              <Empty>
                {status || pageId ? (
                  'Таких абонементов нет.'
                ) : (
                  <>
                    Здесь появятся пакеты, которые клиенты купят при записи или которые вы выдадите сами. Предложения пакетов настраиваются в{' '}
                    <Link className="link" to="/booking">
                      «Записи»
                    </Link>{' '}
                    → «Настроить».
                  </>
                )}
              </Empty>
            )}
            {list?.map((p) => (
              <PackageRow key={p.id} pkg={p} onOpen={() => setOpenId(p.id)} />
            ))}
          </div>
        </section>
      </div>

      <PackageSheet pkg={opened} api={api} onClose={() => setOpenId(null)} onChanged={(p, text) => { put(p); if (text) notify(text); void reload(); }} />
      <IssueSheet
        open={issuing}
        api={api}
        pages={pages ?? []}
        onClose={() => setIssuing(false)}
        onIssued={(issued) => {
          setList((l) => [issued.package, ...(l ?? []).filter((x) => x.id !== issued.package.id)]);
          void reload();
        }}
      />
    </div>
  );
}

// ------------------------------------------------------------------ список

function ProgressBar({ pkg, label }: { pkg: OwnerPackage; label?: string }) {
  const { used, total, ratio } = packageProgress(pkg);
  return (
    <div className="pkg-bar" role="meter" aria-valuemin={0} aria-valuemax={total} aria-valuenow={used} aria-label={label ?? `Использовано ${used} из ${total}`}>
      <span style={{ width: `${Math.round(ratio * 100)}%` }} />
    </div>
  );
}

function PackageRow({ pkg, onOpen }: { pkg: OwnerPackage; onOpen: () => void }) {
  const st = packageStatusView(pkg.status, 'owner');
  const pay = pkg.payment && (pkg.payment.status === 'pending' || pkg.payment.status === 'claimed') ? paymentLabel(pkg.payment.status, 'owner') : null;
  const { used, total } = packageProgress(pkg);
  return (
    <button type="button" className="list-row" style={{ minWidth: 0 }} onClick={onOpen}>
      <Avatar name={pkg.guest_name || '?'} size={40} />
      <div className="body">
        <div className="row" style={{ gap: 8, justifyContent: 'space-between' }}>
          <span className="ellipsis" style={{ fontWeight: 600, minWidth: 0 }}>
            {pkg.guest_name}
            <span className="muted" style={{ fontWeight: 400 }}>
              {' '}
              · {pkg.title}
            </span>
          </span>
          <span className="pkg-left num" aria-label={leftLabel(pkg)}>
            {pkg.sessions_left}
            <small>/{total}</small>
          </span>
        </div>
        <ProgressBar pkg={pkg} />
        <div className="small muted">
          {used} из {total} использовано · {validityLabel(pkg)}
        </div>
        <div className="row wrap" style={{ gap: 6, marginTop: 6 }}>
          <StatusPill status={st} />
          {pay && <StatusPill status={{ ...pay, label: `${pay.label} · ${formatMoney(pkg.payment!.amount, pkg.payment!.currency)}` }} />}
          {pkg.status === 'active' && pkg.sessions_left <= 1 && <StatusPill status={{ label: pkg.sessions_left === 1 ? 'Последнее занятие' : 'Продлить', tone: 'new', icon: 'bell' }} />}
        </div>
      </div>
    </button>
  );
}

// ------------------------------------------------------------------ карточка пакета

function PackageSheet({ pkg, api, onClose, onChanged }: { pkg: OwnerPackage | null; api: PackageApi; onClose: () => void; onChanged: (pkg: OwnerPackage, text?: string) => void }) {
  const [busy, setBusy] = useState('');
  const [error, setError] = useState('');
  const [confirming, setConfirming] = useState(false);
  const close = () => {
    setConfirming(false);
    setError('');
    onClose();
  };
  if (!pkg) return <Sheet open={false} onClose={close} label="Абонемент">{null}</Sheet>;

  const { used, total, left } = packageProgress(pkg);
  const st = packageStatusView(pkg.status, 'owner');
  const payment = pkg.payment;
  const payView = payment ? paymentLabel(payment.status, 'owner') : null;
  const actions = ownerPaymentActions(payment);
  const cancelled = pkg.status === 'cancelled';

  const run = async (key: string, action: () => Promise<{ package: OwnerPackage }>, text?: string) => {
    setBusy(key);
    setError('');
    try {
      const reply = await action();
      onChanged(reply.package, text);
      setConfirming(false);
    } catch (e) {
      setError(bookingOwnerErrorText(e));
    } finally {
      setBusy('');
    }
  };

  return (
    <Sheet open onClose={close} leading={<Avatar name={pkg.guest_name || '?'} size={52} />} title={pkg.guest_name || 'Клиент'} subtitle={[pkg.title, pkg.page_title].filter(Boolean).join(' · ')}>
      <div className="card pkg-meter">
        <div className="row" style={{ justifyContent: 'space-between', alignItems: 'flex-end' }}>
          <div>
            <div className="big num">{left}</div>
            <div className="small muted">
              {plural(left, 'занятие осталось', 'занятия осталось', 'занятий осталось')} из {total}
            </div>
          </div>
          <StatusPill status={st} />
        </div>
        <ProgressBar pkg={pkg} />
        <div className="small muted">
          Использовано {used} · {validityLabel(pkg)}
        </div>
      </div>

      <div className="group">
        <div className="group-head">Поправить остаток</div>
        <div className="group-body">
          <div className="cell">
            <span className="lab">
              Занятий в пакете: {total}
              <small>«+1» — подарить или вернуть занятие, «−1» — списать вручную</small>
            </span>
            <span className="row" style={{ gap: 6 }}>
              <button type="button" className="icon-btn stepper" aria-label="Списать одно занятие" disabled={cancelled || left <= 0 || Boolean(busy)} onClick={() => void run('minus', () => api.adjust(pkg.id, -1), 'Списано одно занятие')}>
                {busy === 'minus' ? <Spinner small /> : <Minus size={18} />}
              </button>
              <button type="button" className="icon-btn stepper" aria-label="Добавить одно занятие" disabled={cancelled || total >= 1000 || Boolean(busy)} onClick={() => void run('plus', () => api.adjust(pkg.id, 1), 'Добавлено одно занятие')}>
                {busy === 'plus' ? <Spinner small /> : <Plus size={18} />}
              </button>
            </span>
          </div>
        </div>
      </div>

      <div className="group-body">
        <div className="cell">
          <span className="lab">Цена</span>
          <span className="val num">{pkg.price ? formatMoney(pkg.price, pkg.currency) : 'Бесплатно'}</span>
        </div>
        {payment && payView && (
          <div className="cell">
            <span className="lab">
              Оплата
              <small>
                {payment.paid_at ? `оплачено ${when(payment.paid_at)}` : payment.due_at ? `до ${when(payment.due_at)}` : payment.method === 'yookassa' ? 'через ЮKassa' : 'отметьте, когда получите'}
              </small>
            </span>
            <StatusPill status={payView} />
          </div>
        )}
        {pkg.guest_email && (
          <div className="cell">
            <span className="lab">Email</span>
            <span className="val ellipsis" style={{ maxWidth: '62%' }}>
              {pkg.guest_email}
            </span>
          </div>
        )}
        {pkg.cc_email && (
          <div className="cell">
            <span className="lab">Копия напоминаний</span>
            <span className="val ellipsis" style={{ maxWidth: '55%' }}>
              {pkg.cc_email}
            </span>
          </div>
        )}
        <div className="cell">
          <span className="lab">{pkg.offer_id ? 'Куплен' : 'Выдан вами'}</span>
          <span className="val">{when(pkg.created_at)}</span>
        </div>
      </div>

      {error && <div className="notice error">{error}</div>}

      {actions.length > 0 && !cancelled && (
        <div className="stack" style={{ gap: 8 }}>
          {actions.map((a, i) => (
            <button
              key={a}
              type="button"
              className={`btn btn-lg btn-block ${i === 0 && a === 'paid' ? 'btn-primary' : a === 'refunded' ? 'btn-danger' : 'btn-secondary'}`}
              disabled={Boolean(busy)}
              onClick={() => void run(`pay-${a}`, () => api.paymentSet(pkg.id, a as OwnerPaymentStatus), a === 'paid' ? 'Оплата отмечена — клиенту ушло письмо' : undefined)}
            >
              {busy === `pay-${a}` ? <Spinner small /> : a === 'paid' ? <Check size={18} /> : null} {OWNER_PAYMENT_ACTION[a]}
            </button>
          ))}
        </div>
      )}

      {!cancelled &&
        (confirming ? (
          <div className="notice error" style={{ flexWrap: 'wrap', alignItems: 'center' }}>
            <span style={{ flex: '1 1 200px' }}>
              Отменить абонемент? По ссылке больше не записаться; уже сделанные записи останутся.
              {payment?.method === 'yookassa' && payment.status === 'paid' && used === 0 ? ' Оплата вернётся клиенту.' : ''}
            </span>
            <button type="button" className="btn btn-sm btn-primary" style={{ background: 'var(--danger)' }} disabled={Boolean(busy)} onClick={() => void run('cancel', () => api.cancel(pkg.id), 'Абонемент отменён')}>
              {busy === 'cancel' ? <Spinner small /> : 'Да, отменить'}
            </button>
            <button type="button" className="btn btn-sm btn-ghost" onClick={() => setConfirming(false)}>
              Нет
            </button>
          </div>
        ) : (
          <button type="button" className="btn btn-ghost btn-block" style={{ color: 'var(--danger)' }} onClick={() => setConfirming(true)}>
            <Ban size={16} /> Отменить абонемент
          </button>
        ))}
    </Sheet>
  );
}

// ------------------------------------------------------------------ выдача

const emptyIssue = (pages: BookingPage[]): IssueForm => {
  const page = pages.find((p) => p.status === 'active') ?? pages[0];
  const offer = page?.packages?.[0];
  return {
    page_id: page?.id ?? 0,
    offer_id: offer?.id ?? 0,
    title: '',
    sessions: 8,
    price: 0,
    valid_days: OFFER_DEFAULT_DAYS,
    guest_name: '',
    guest_email: '',
    cc_email: '',
    paid: false,
  };
};

function IssueSheet({ open, api, pages, onClose, onIssued }: { open: boolean; api: PackageApi; pages: BookingPage[]; onClose: () => void; onIssued: (issued: Issued) => void }) {
  const [form, setForm] = useState<IssueForm>(() => emptyIssue(pages));
  const [errors, setErrors] = useState<IssueErrors & { form?: string }>({});
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState<Issued | null>(null);
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    if (!open) return;
    setForm(emptyIssue(pages));
    setErrors({});
    setDone(null);
    setCopied(false);
  }, [open]); // eslint-disable-line react-hooks/exhaustive-deps

  const page = pages.find((p) => p.id === form.page_id);
  const offers = page?.packages ?? [];
  const offer = offers.find((o) => o.id === form.offer_id);
  const currency = page?.currency ?? 'RUB';
  const price = offer ? offer.price : form.price;

  const set = <K extends keyof IssueForm>(key: K, value: IssueForm[K]) => {
    setForm((f) => ({ ...f, [key]: value }));
    setErrors((e) => (e.form ? { ...e, form: undefined } : e));
  };

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const next = issueErrors(form);
    setErrors(next);
    if (hasErrors(next)) return;
    setBusy(true);
    try {
      const base = { page_id: form.page_id, guest_name: form.guest_name.trim(), guest_email: form.guest_email.trim() || undefined, cc_email: form.cc_email.trim() || undefined, paid: price > 0 && form.paid };
      const reply = await api.issue(form.offer_id ? { ...base, offer_id: form.offer_id } : { ...base, title: form.title.trim(), sessions: form.sessions, price: form.price, valid_days: form.valid_days });
      setDone(reply);
      onIssued(reply);
    } catch (error) {
      setErrors({ form: bookingOwnerErrorText(error) });
    } finally {
      setBusy(false);
    }
  };

  if (done) {
    const link = publicLink(packagePath(done.token), done.url, here());
    const text = link ?? `#${packagePath(done.token)}`;
    const copy = async () => {
      const ok = await copyText(text);
      setCopied(ok);
      if (ok) setTimeout(() => setCopied(false), 2000);
    };
    return (
      <Sheet open={open} onClose={onClose} title="Абонемент выдан" subtitle={`${done.package.guest_name} · ${done.package.title}`}>
        <div className="stack">
          <div className="card pkg-meter">
            <div className="big num">{done.package.sessions_left}</div>
            <div className="small muted">
              {sessionsLabel(done.package.sessions_total)} · {validityLabel(done.package)}
            </div>
          </div>
          <span className="small muted">
            По этой ссылке клиент видит остаток и записывается по абонементу.{done.package.guest_email ? ' Она уже ушла клиенту письмом.' : ''} Ссылка показывается один раз — сохраните или отправьте её сейчас.
          </span>
          <input className="input mono" readOnly value={text} aria-label="Ссылка абонемента" onFocus={(e) => e.currentTarget.select()} />
          {!link && <div className="notice warn">Сервер не знает свой публичный адрес — полную ссылку не собрать. Задайте на сервере SELFVOY_PUBLIC_URL.</div>}
          <div className="grid-2">
            <button type="button" className="btn btn-primary btn-lg" onClick={copy}>
              <Copy size={17} /> {copied ? 'Скопировано' : 'Скопировать'}
            </button>
            {link && canShare() ? (
              <button type="button" className="btn btn-secondary btn-lg" onClick={() => navigator.share({ title: done.package.title, text: `${done.package.title}: ваш абонемент`, url: link }).catch(() => undefined)}>
                <Share2 size={17} /> Поделиться
              </button>
            ) : (
              <button type="button" className="btn btn-secondary btn-lg" onClick={onClose}>
                Готово
              </button>
            )}
          </div>
        </div>
      </Sheet>
    );
  }

  return (
    <Sheet open={open} onClose={onClose} title="Выдать абонемент" subtitle="Продажа вне сайта: пакет сразу действует">
      {pages.length === 0 ? (
        <div className="stack">
          <Empty>Абонемент привязан к странице записи — сначала создайте её.</Empty>
          <Link className="btn btn-primary btn-lg btn-block" to="/booking?new=1" onClick={onClose}>
            Новая страница записи
          </Link>
        </div>
      ) : (
        <form className="stack-lg" style={{ gap: 22 }} onSubmit={submit} noValidate>
          <Group head="Пакет" foot={errors.page || errors.offer ? <span className="field-error">{errors.page || errors.offer}</span> : offer ? offerSummary(offer, currency) : 'Своё название и число занятий — без предложения на странице.'}>
            {pages.length > 1 && (
              <label className="cell">
                <span className="lab">Страница</span>
                <select
                  className="input"
                  style={{ width: 'auto', maxWidth: '60%', height: 38 }}
                  value={form.page_id}
                  onChange={(e) => {
                    const next = pages.find((p) => p.id === Number(e.target.value));
                    setForm((f) => ({ ...f, page_id: Number(e.target.value), offer_id: next?.packages?.[0]?.id ?? 0 }));
                  }}
                >
                  {pages.map((p) => (
                    <option key={p.id} value={p.id}>
                      {p.title}
                    </option>
                  ))}
                </select>
              </label>
            )}
            <label className="cell">
              <span className="lab">Предложение</span>
              <select className="input" style={{ width: 'auto', maxWidth: '60%', height: 38 }} value={form.offer_id} onChange={(e) => set('offer_id', Number(e.target.value))}>
                {offers.map((o) => (
                  <option key={o.id} value={o.id}>
                    {o.title} — {formatMoney(o.price, currency)}
                  </option>
                ))}
                <option value={0}>Своё…</option>
              </select>
            </label>
            {!form.offer_id && (
              <>
                <div className="cell stacked">
                  <input className="input" placeholder="Название, например «Абонемент на месяц»" aria-label="Название пакета" value={form.title} maxLength={OFFER_TITLE_MAX} onChange={(e) => set('title', e.target.value)} />
                </div>
                <div className="cell stacked">
                  <div className="offer-grid">
                    <label className="offer-field">
                      <span className="small muted">Занятий</span>
                      <input className="input num" type="number" inputMode="numeric" min={1} max={100} value={Number.isNaN(form.sessions) ? '' : form.sessions} onChange={(e) => set('sessions', e.target.value === '' ? Number.NaN : Number(e.target.value))} />
                    </label>
                    <label className="offer-field">
                      <span className="small muted">Цена</span>
                      <MoneyInput width="100%" label="Цена пакета" value={form.price} currency={currency} onChange={(v) => set('price', v)} />
                    </label>
                    <label className="offer-field">
                      <span className="small muted">Срок, дней</span>
                      <input className="input num" type="number" inputMode="numeric" min={7} max={365} value={Number.isNaN(form.valid_days) ? '' : form.valid_days} onChange={(e) => set('valid_days', e.target.value === '' ? Number.NaN : Number(e.target.value))} />
                    </label>
                  </div>
                </div>
              </>
            )}
            {price > 0 && (
              <div className="cell">
                <span className="lab">
                  Оплата уже получена<small>{form.paid ? 'пакет сразу «оплачен»' : 'иначе отметите оплату позже'}</small>
                </span>
                <Switch on={form.paid} onChange={(v) => set('paid', v)} label="Оплата уже получена" />
              </div>
            )}
          </Group>

          <Group head="Клиент" foot="Клиенту (и копии) придёт письмо со ссылкой абонемента.">
            <div className="cell stacked">
              <Field label="Имя" error={errors.guest_name}>
                <input className={`input ${errors.guest_name ? 'invalid' : ''}`} value={form.guest_name} maxLength={GUEST_NAME_MAX} onChange={(e) => set('guest_name', e.target.value)} />
              </Field>
              <Field label="Email" error={errors.guest_email} hint="Необязательно">
                <input className={`input ${errors.guest_email ? 'invalid' : ''}`} type="email" inputMode="email" placeholder="client@example.com" value={form.guest_email} maxLength={EMAIL_MAX} onChange={(e) => set('guest_email', e.target.value)} />
              </Field>
              <Field label="Копия напоминаний (например, родителю)" error={errors.cc_email} hint="Необязательно">
                <input className={`input ${errors.cc_email ? 'invalid' : ''}`} type="email" inputMode="email" value={form.cc_email} maxLength={EMAIL_MAX} onChange={(e) => set('cc_email', e.target.value)} />
              </Field>
            </div>
          </Group>

          {errors.form && (
            <div className="notice error" role="alert">
              {errors.form}
            </div>
          )}
          <button className="btn btn-primary btn-lg btn-block" type="submit" disabled={busy}>
            {busy ? <Spinner small /> : <Package size={18} />} Выдать абонемент
          </button>
        </form>
      )}
    </Sheet>
  );
}
