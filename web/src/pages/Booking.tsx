/*
  «Запись» — страницы записи владельца (booking.*).

  Постоянная публичная ссылка #/b/<slug>: гость без Selfvoy сам выбирает свободное
  время, при желании — сразу еженедельную серию. Здесь владелец заводит и
  настраивает страницы (услуга, цена, предоплата, политика отмены, напоминания
  гостю, подпись Selfvoy), делает пост со свободными окнами, ставит страницу на
  паузу и видит записи гостей с подтверждением и оплатой. Список обновляется по
  уведомлениям booking.*.

  Пакеты и абонементы (F5): группа формы «Пакеты и абонементы» (до 5 предложений,
  Pro) и переход на экран «Абонементы» (pages/Packages.tsx); у записи по пакету —
  отметка «Абонемент».
*/
import { CalendarClock, ChevronRight, Copy, Megaphone, Package, Pause, Pencil, Play, Plus, Share2, Trash2 } from 'lucide-react';
import { useEffect, useMemo, useState, type FormEvent, type ReactNode } from 'react';
import { Link, useSearchParams } from 'react-router-dom';
import { BookingSheet } from '../components/BookingSheet';
import { MoneyInput } from '../components/MoneyInput';
import { Avatar, Empty, Group, Logo, ProBadge, Segmented, Sheet, Spinner, StatusPill, Switch } from '../components/ui';
import { SkeletonList } from '../components/Skeleton';
import { EmptyState } from '../components/EmptyState';
import {
  bookingOwnerErrorText,
  bookingStatus,
  changedFields,
  DESCRIPTION_MAX,
  PAGE_DEFAULTS,
  PAGE_STATUS_LABEL,
  pageFields,
  pageFormErrors,
  pagePath,
  PAYMENT_NOTE_MAX,
  PAYMENT_URL_MAX,
  POLICY_MAX,
  POST_FORMATS,
  publicLink,
  sortedOccurrences,
  TITLE_MAX,
  withPostLink,
  type PageFormErrors,
} from '../lib/booking';
import { canShare, copyText } from '../lib/clipboard';
import { plural, when } from '../lib/format';
import { isProRequired, planView } from '../lib/integrations';
import { hasErrors } from '../lib/invite';
import { CURRENCIES, formatMoney } from '../lib/money';
import { cleanOffers, newOffer, OFFER_TITLE_MAX, OFFERS_MAX, offersError, perSessionLabel, sessionsLabel } from '../lib/packages';
import { paymentLabel } from '../lib/payment';
import { makeIntegrationsApi } from '../net/integrationsApi';
import type { Booking, BookingApi, BookingPage as Page, PageForm as Form, PostFormat } from '../net/bookingApi';
import type { PackageOffer } from '../net/packageApi';
import { useData } from '../state/data';
import { useOwnerBookings } from '../state/owner';
import { useSession } from '../state/session';

const DURATIONS = [
  { value: '15', label: '15 мин' },
  { value: '30', label: '30 мин' },
  { value: '45', label: '45 мин' },
  { value: '60', label: '1 час' },
  { value: '90', label: '1,5 ч' },
];

const HORIZONS = [
  { value: '7', label: 'Неделя' },
  { value: '14', label: '2 недели' },
  { value: '30', label: 'Месяц' },
  { value: '60', label: '2 месяца' },
];

const PREPAY = [
  { value: 'none', label: 'Нет' },
  { value: 'full', label: 'Вся сумма' },
  { value: 'deposit', label: 'Депозит' },
] as const;

const LATE = [
  { value: 'allow', label: 'Разрешить без возврата' },
  { value: 'block', label: 'Запретить' },
] as const;

const here = () => (typeof location === 'undefined' ? null : location);

export function BookingPage() {
  const { notify } = useData();
  const owner = useOwnerBookings();
  const { api, pages, bookings, error } = owner;
  const [params, setParams] = useSearchParams();
  const [editing, setEditing] = useState<Page | 'new' | null>(null);
  const [posting, setPosting] = useState<Page | null>(null);
  const [open, setOpen] = useState<number | null>(null);
  const [filter, setFilter] = useState<'upcoming' | 'all'>('upcoming');

  // Быстрые действия «+»: ?new=1 — новая страница, ?post=1 — пост первой открытой страницы.
  useEffect(() => {
    if (params.get('new') === '1') {
      setEditing('new');
      setParams({}, { replace: true });
    } else if (params.get('post') === '1' && pages) {
      const first = pages.find((p) => p.status === 'active');
      if (first) setPosting(first);
      else notify(pages.length ? 'Все страницы на паузе — откройте одну, чтобы сделать пост' : 'Сначала создайте страницу записи', 'error');
      setParams({}, { replace: true });
    }
  }, [params, pages, setParams, notify]);

  const setStatus = async (page: Page, status: 'active' | 'paused') => {
    try {
      const reply = await api.updatePage(page.id, { status });
      owner.putPage(reply.page);
      void owner.reload();
      notify(status === 'paused' ? 'Запись на паузе — новые гости не смогут записаться' : 'Запись снова открыта');
    } catch (e) {
      notify(bookingOwnerErrorText(e), 'error');
    }
  };

  const remove = async (page: Page) => {
    try {
      await api.deletePage(page.id);
      owner.dropPage(page.id);
      void owner.reload();
      notify('Страница удалена. Уже сделанные записи остаются в силе.');
    } catch (e) {
      notify(bookingOwnerErrorText(e), 'error');
    }
  };

  const now = new Date();
  const shown = (bookings ?? []).filter((b) => filter === 'all' || (b.status === 'active' && b.occurrences.some((o) => o.status === 'active' && Date.parse(o.end || o.start) > now.getTime())));
  const opened = open ? (bookings ?? []).find((b) => b.id === open) ?? null : null;

  return (
    <div className="page">
      <header className="page-head">
        <div>
          <h1 className="title-xl">Запись</h1>
          <div className="sub">Постоянная ссылка: клиенты сами выбирают свободное время в вашем расписании.</div>
        </div>
        {pages !== null && (
          <div className="row" style={{ gap: 8 }}>
            <Link className="btn btn-secondary" to="/packages">
              <Package size={17} /> Абонементы
            </Link>
            <button className="btn btn-primary" onClick={() => setEditing('new')}>
              <CalendarClock size={17} /> <span className="hide-phone">Новая страница</span>
              <span className="hide-desktop">Новая</span>
            </button>
          </div>
        )}
      </header>

      <div className="stack-lg">
        {error && <div className="notice error">{error}</div>}

        <section className="stack">
          <div className="sec-h" style={{ marginTop: 0 }}>
            <h2>Страницы</h2>
          </div>
          {pages === null && !error && (
            <SkeletonList rows={2} />
          )}
          {pages?.length === 0 && (
            <EmptyState
              icon={CalendarClock}
              title="Нет страниц записи"
              desc="Создайте первую страницу — например, «Консультация, 50 минут» — и отправьте ссылку клиентам."
              action="Создать страницу"
              onAction={() => setEditing('new')}
            />
          )}
          {pages?.map((page) => (
            <PageCard key={page.id} page={page} onEdit={() => setEditing(page)} onPost={() => setPosting(page)} onStatus={setStatus} onDelete={remove} />
          ))}
        </section>

        <section className="stack">
          <div className="sec-h" style={{ marginTop: 0 }}>
            <h2>Записи клиентов</h2>
            <Segmented
              label="Какие записи показать"
              value={filter}
              onChange={setFilter}
              options={[
                { value: 'upcoming', label: 'Предстоящие' },
                { value: 'all', label: 'Все' },
              ]}
            />
          </div>
          <div className="card flush list">
            {bookings === null && !error && (
              <SkeletonList rows={3} />
            )}
            {bookings !== null && shown.length === 0 && <Empty>{filter === 'upcoming' ? 'Предстоящих записей нет.' : 'Здесь появятся люди, которые записались по вашим ссылкам.'}</Empty>}
            {shown.map((b) => (
              <BookingRow key={b.id} booking={b} onOpen={() => setOpen(b.id)} />
            ))}
          </div>
        </section>
      </div>

      <PageFormSheet
        api={api}
        page={editing === 'new' ? null : editing}
        open={editing !== null}
        onClose={() => setEditing(null)}
        onSaved={(page) => {
          owner.putPage(page, editing === 'new');
          setEditing(null);
          void owner.reload();
          notify(editing === 'new' ? 'Страница создана — поделитесь ссылкой' : 'Сохранено');
        }}
      />
      <PostSheet api={api} page={posting} onClose={() => setPosting(null)} />
      <BookingSheet
        booking={opened}
        pages={pages}
        api={api}
        onClose={() => setOpen(null)}
        onChanged={(b) => {
          owner.putBooking(b);
          void owner.reload();
        }}
      />
    </div>
  );
}

// ------------------------------------------------------------------ страница

function pageFacts(page: Page): string {
  const facts = [`${page.duration_minutes} мин`];
  if (page.price) facts.push(formatMoney(page.price, page.currency));
  if (page.prepay === 'full' && page.price) facts.push('предоплата 100%');
  else if (page.prepay === 'deposit' && page.deposit) facts.push(`депозит ${formatMoney(page.deposit, page.currency)}`);
  if (page.cancel_hours) facts.push(`отмена за ${page.cancel_hours} ч`);
  facts.push(page.weekends ? 'и в выходные' : 'будни');
  if (page.max_series > 1) facts.push(`серия до ${page.max_series}`);
  const offers = page.packages?.length ?? 0;
  if (offers) facts.push(`${offers} ${plural(offers, 'пакет', 'пакета', 'пакетов')}`);
  return facts.join(' · ');
}

function PageCard({
  page,
  onEdit,
  onPost,
  onStatus,
  onDelete,
}: {
  page: Page;
  onEdit: () => void;
  onPost: () => void;
  onStatus: (page: Page, status: 'active' | 'paused') => Promise<void>;
  onDelete: (page: Page) => Promise<void>;
}) {
  const [copied, setCopied] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const link = publicLink(pagePath(page.slug), page.url, here());
  const text = link ?? `#${pagePath(page.slug)}`;
  const active = page.status === 'active';

  const copy = async () => {
    const ok = await copyText(text);
    setCopied(ok);
    if (ok) setTimeout(() => setCopied(false), 2000);
  };

  const run = async (action: () => Promise<void>) => {
    setBusy(true);
    try {
      await action();
    } finally {
      setBusy(false);
      setConfirming(false);
    }
  };

  return (
    <div className="group-body page-card">
      <div className="cell has-ico">
        <span className="ico ink" style={{ color: '#fff' }}>
          <Logo size={20} mono />
        </span>
        <span className="lab">
          <b style={{ fontWeight: 650 }}>{page.title}</b>
          <small>
            {page.upcoming > 0 ? `предстоит ${page.upcoming} ${plural(page.upcoming, 'встреча', 'встречи', 'встреч')}` : 'пока никто не записался'}
          </small>
        </span>
        <StatusPill status={active ? { label: PAGE_STATUS_LABEL.active, tone: 'green', icon: 'check' } : { label: PAGE_STATUS_LABEL[page.status] ?? page.status, tone: 'amber', icon: 'clock' }} />
      </div>
      <div className="cell stacked">
        <span className="small muted">{pageFacts(page)}</span>
        {page.description && <span className="small muted invite-note">{page.description}</span>}
        <div className="row" style={{ gap: 8 }}>
          <code className="linkbox ellipsis" style={{ flex: 1, minWidth: 0, whiteSpace: 'nowrap' }} title={text}>
            {text}
          </code>
          <button type="button" className="btn btn-secondary btn-sm" onClick={copy} aria-label="Скопировать ссылку">
            <Copy size={15} /> <span>{copied ? 'Скопировано' : 'Скопировать'}</span>
          </button>
          {link && canShare() && (
            <button
              type="button"
              className="btn btn-secondary btn-sm"
              aria-label="Поделиться"
              onClick={() => navigator.share({ title: page.title, text: `${page.title} — запишитесь на удобное время`, url: link }).catch(() => undefined)}
            >
              <Share2 size={15} />
            </button>
          )}
        </div>
        {!link && (
          <div className="notice warn">Сервер не знает свой публичный адрес, а приложение открыто локально — полную ссылку не собрать. Задайте на сервере SELFVOY_PUBLIC_URL.</div>
        )}
      </div>
      <div className="cell">
        {confirming ? (
          <div className="row wrap" style={{ gap: 8, width: '100%' }}>
            <span className="small" style={{ marginRight: 'auto' }}>
              Удалить страницу? Записи останутся.
            </span>
            <button className="btn btn-danger btn-sm" disabled={busy} onClick={() => run(() => onDelete(page))}>
              {busy ? <Spinner small /> : 'Удалить'}
            </button>
            <button className="btn btn-ghost btn-sm" disabled={busy} onClick={() => setConfirming(false)}>
              Нет
            </button>
          </div>
        ) : (
          <div className="row page-acts" style={{ gap: 6, width: '100%' }}>
            <button className="btn btn-ghost btn-sm" onClick={onEdit}>
              <Pencil size={15} /> Настроить
            </button>
            {active && (
              <button className="btn btn-ghost btn-sm" onClick={onPost} aria-label="Пост с окнами">
                <Megaphone size={15} />
                <span>
                  Пост<span className="hide-phone"> с окнами</span>
                </span>
              </button>
            )}
            <span className="spacer" />
            {active ? (
              <button className="btn btn-ghost btn-sm" disabled={busy} onClick={() => run(() => onStatus(page, 'paused'))} aria-label="Поставить на паузу">
                <Pause size={15} /> <span className="hide-phone">Пауза</span>
              </button>
            ) : (
              <button className="btn btn-ghost btn-sm" disabled={busy} onClick={() => run(() => onStatus(page, 'active'))}>
                <Play size={15} /> Открыть
              </button>
            )}
            <button className="btn btn-ghost btn-sm" style={{ color: 'var(--danger)' }} onClick={() => setConfirming(true)} aria-label="Удалить страницу">
              <Trash2 size={15} />
            </button>
          </div>
        )}
      </div>
    </div>
  );
}

// ------------------------------------------------------------------ форма страницы

function NumberCell({ label, hint, value, min, max, onChange, suffix }: { label: string; hint?: string; value: number; min: number; max: number; onChange: (v: number) => void; suffix?: string }) {
  return (
    <label className="cell">
      <span className="lab">
        {label}
        {hint && <small>{hint}</small>}
      </span>
      <span className="input-affix" style={{ width: suffix ? 128 : 96 }}>
        <input
          className="input num"
          type="number"
          inputMode="numeric"
          min={min}
          max={max}
          step={1}
          style={{ height: 38, textAlign: 'right', paddingRight: suffix ? 44 : 14 }}
          value={Number.isNaN(value) ? '' : value}
          onChange={(e) => onChange(e.target.value === '' ? Number.NaN : Number(e.target.value))}
        />
        {suffix && <span className="affix small">{suffix}</span>}
      </span>
    </label>
  );
}

function SwitchCell({ label, hint, on, onChange, badge }: { label: string; hint?: ReactNode; on: boolean; onChange: (v: boolean) => void; badge?: ReactNode }) {
  return (
    <div className="cell">
      <span className="lab">
        <span className="row" style={{ gap: 8 }}>
          {label} {badge}
        </span>
        {hint && <small>{hint}</small>}
      </span>
      <Switch on={on} onChange={onChange} label={label} />
    </div>
  );
}

function PageFormSheet({ api, page, open, onSaved, onClose }: { api: BookingApi; page: Page | null; open: boolean; onSaved: (page: Page) => void; onClose: () => void }) {
  const { socket, user } = useSession();
  const integrations = useMemo(() => makeIntegrationsApi(socket), [socket]);
  const initial = useMemo(() => (page ? pageFields(page) : PAGE_DEFAULTS), [page]);
  const [form, setForm] = useState<Form>(initial);
  const [errors, setErrors] = useState<PageFormErrors & { form?: string; pro?: boolean }>({});
  const [busy, setBusy] = useState(false);
  const [yookassa, setYookassa] = useState(false);
  const plan = planView(user?.plan);
  const proLocked = Boolean(user?.plan?.enforced && !plan?.pro);

  useEffect(() => {
    if (!open) return;
    setForm(initial);
    setErrors({});
    integrations
      .paymentsStatus()
      .then((s) => setYookassa(Boolean(s.connected)))
      .catch(() => setYookassa(false));
  }, [open, initial, integrations]);

  const set = <K extends keyof Form>(key: K, value: Form[K]) => {
    setForm((f) => ({ ...f, [key]: value }));
    setErrors((e) => (e.form || e.pro ? { ...e, form: undefined, pro: false } : e));
  };

  const durationOptions = DURATIONS.some((d) => d.value === String(form.duration_minutes)) ? DURATIONS : [...DURATIONS, { value: String(form.duration_minutes), label: `${form.duration_minutes} мин` }];
  const horizonOptions = HORIZONS.some((h) => h.value === String(form.days_ahead)) ? HORIZONS : [...HORIZONS, { value: String(form.days_ahead), label: `${form.days_ahead} дн.` }];

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    const next = pageFormErrors(form, { yookassa });
    setErrors(next);
    if (hasErrors(next)) return;
    setBusy(true);
    try {
      const clean: Form = {
        ...form,
        title: form.title.trim(),
        description: (form.description ?? '').trim(),
        payment_url: form.payment_url.trim(),
        payment_note: form.payment_note.trim(),
        policy_text: form.policy_text.trim(),
        deposit: form.prepay === 'deposit' ? form.deposit : initial.deposit,
        packages: cleanOffers(form.packages),
      };
      const reply = page ? await api.updatePage(page.id, changedFields(initial, clean)) : await api.createPage(clean);
      onSaved(reply.page);
    } catch (error) {
      setErrors({ form: bookingOwnerErrorText(error), pro: isProRequired(error) });
    } finally {
      setBusy(false);
    }
  };

  const total = form.prepay === 'deposit' ? form.deposit : form.price;

  return (
    <Sheet open={open} onClose={onClose} title={page ? 'Страница записи' : 'Новая страница записи'} subtitle={page ? page.title : 'Ссылка, по которой клиенты записываются сами'}>
      <form className="stack-lg" style={{ gap: 22 }} onSubmit={submit} noValidate>
        <Group head="Услуга" foot={errors.title || errors.description ? <span className="field-error">{errors.title || errors.description}</span> : 'Гости видят только свободное время в ваших рабочих часах — без названий событий.'}>
          <div className="cell stacked">
            <input className={`input ${errors.title ? 'invalid' : ''}`} placeholder="Название, например «Урок математики»" aria-label="Название" value={form.title} maxLength={TITLE_MAX} onChange={(e) => set('title', e.target.value)} />
            <textarea className="textarea" style={{ minHeight: 64 }} placeholder="Описание для клиентов — необязательно" aria-label="Описание" value={form.description ?? ''} maxLength={DESCRIPTION_MAX} onChange={(e) => set('description', e.target.value)} />
          </div>
          <div className="cell stacked">
            <span className="small muted">Длительность</span>
            <Segmented label="Длительность" value={String(form.duration_minutes)} onChange={(v) => set('duration_minutes', Number(v))} options={durationOptions} />
          </div>
        </Group>

        <Group head="Расписание" foot={errors.numbers ? <span className="field-error">{errors.numbers}</span> : undefined}>
          <div className="cell stacked">
            <span className="small muted">Запись открыта на</span>
            <Segmented label="Запись открыта на" value={String(form.days_ahead)} onChange={(v) => set('days_ahead', Number(v))} options={horizonOptions} />
          </div>
          <NumberCell label="Встреч в день" hint="не больше" value={form.max_per_day} min={1} max={16} onChange={(v) => set('max_per_day', v)} />
          <NumberCell label="Запас до встречи" hint="записаться можно не позже" value={form.notice_hours} min={0} max={168} suffix="ч" onChange={(v) => set('notice_hours', v)} />
          <NumberCell label="Серия" hint="больше 1 — встречи раз в неделю" value={form.max_series} min={1} max={12} onChange={(v) => set('max_series', v)} />
          <SwitchCell label="Выходные" hint={form.weekends ? 'Будни и выходные' : 'Только будни'} on={form.weekends} onChange={(v) => set('weekends', v)} />
        </Group>

        <Group head="Цена и оплата" foot={errors.price || errors.payment ? <span className="field-error">{errors.price || errors.payment}</span> : form.prepay === 'none' ? 'Цена видна клиенту на странице записи. Без предоплаты — бесплатно.' : `Время держится ${form.pay_within_minutes} мин — не оплачено к сроку, запись отменится сама.`}>
          <div className="cell">
            <span className="lab">Цена</span>
            <MoneyInput label="Цена" value={form.price} currency={form.currency} invalid={Boolean(errors.price)} onChange={(v) => set('price', v)} />
          </div>
          <label className="cell">
            <span className="lab">Валюта</span>
            <select className="input" style={{ width: 'auto', height: 38 }} value={form.currency} onChange={(e) => set('currency', e.target.value as Form['currency'])}>
              {CURRENCIES.map((c) => (
                <option key={c.value} value={c.value}>
                  {c.label}
                </option>
              ))}
            </select>
          </label>
          <div className="cell stacked">
            <span className="row small muted" style={{ gap: 8 }}>
              Предоплата <ProBadge />
            </span>
            <Segmented label="Предоплата" value={form.prepay} onChange={(v) => set('prepay', v)} options={PREPAY} />
          </div>
          {form.prepay === 'deposit' && (
            <div className="cell">
              <span className="lab">
                Депозит<small>один раз на запись</small>
              </span>
              <MoneyInput label="Депозит" value={form.deposit} currency={form.currency} invalid={Boolean(errors.price)} onChange={(v) => set('deposit', v)} />
            </div>
          )}
          {form.prepay !== 'none' && (
            <>
              {yookassa ? (
                <div className="cell">
                  <span className="lab">
                    Оплата через ЮKassa<small>гость платит картой или СБП, статус обновится сам</small>
                  </span>
                  <StatusPill status={{ label: 'Подключена', tone: 'green', icon: 'check' }} />
                </div>
              ) : (
                <Link to="/settings?s=integrations" className="cell">
                  <span className="lab">
                    Подключить ЮKassa<small>или укажите ссылку оплаты и реквизиты ниже</small>
                  </span>
                  <span className="val">Настройки</span>
                </Link>
              )}
              <div className="cell stacked">
                <input className="input" type="url" inputMode="url" placeholder="Ссылка оплаты: https://…" aria-label="Ссылка оплаты" value={form.payment_url} maxLength={PAYMENT_URL_MAX} onChange={(e) => set('payment_url', e.target.value)} />
                <input className="input" placeholder="Реквизиты или СБП: +7 900 000-00-00, Т-Банк" aria-label="Реквизиты для оплаты" value={form.payment_note} maxLength={PAYMENT_NOTE_MAX} onChange={(e) => set('payment_note', e.target.value)} />
              </div>
              <NumberCell label="Срок оплаты" hint="15–1440 минут" value={form.pay_within_minutes} min={15} max={1440} suffix="мин" onChange={(v) => set('pay_within_minutes', v)} />
              {total > 0 && (
                <div className="cell">
                  <span className="lab">Клиент заплатит при записи</span>
                  <span className="val num">{formatMoney(total, form.currency)}</span>
                </div>
              )}
            </>
          )}
        </Group>

        <Group head="Отмена и перенос" foot={errors.policy ? <span className="field-error">{errors.policy}</span> : form.cancel_hours === 0 ? 'Клиент может перенести или отменить встречу в любое время.' : `Позже чем за ${form.cancel_hours} ч — ${form.late_cancel === 'block' ? 'только через вас' : 'без возврата оплаты'}.`}>
          <NumberCell label="Бесплатная отмена" hint="не позже чем за N часов; 0 — всегда" value={form.cancel_hours} min={0} max={168} suffix="ч" onChange={(v) => set('cancel_hours', v)} />
          {form.cancel_hours > 0 && (
            <div className="cell stacked">
              <span className="small muted">Позже срока</span>
              <Segmented label="Отмена позже срока" value={form.late_cancel} onChange={(v) => set('late_cancel', v)} options={LATE} />
            </div>
          )}
          <div className="cell stacked">
            <textarea className="textarea" style={{ minHeight: 56 }} placeholder="Текст правил для клиента — необязательно" aria-label="Текст политики отмены" value={form.policy_text} maxLength={POLICY_MAX} onChange={(e) => set('policy_text', e.target.value)} />
          </div>
        </Group>

        <Group head="Клиенту">
          <SwitchCell label="Напоминания" hint="За сутки и за 2 часа — письмом и в Telegram, с кнопкой «Приду»" on={form.guest_reminders} onChange={(v) => set('guest_reminders', v)} />
          <SwitchCell
            label="Скрыть «Запись через Selfvoy»"
            hint={proLocked ? 'Доступно на тарифе Pro' : 'Подпись внизу страницы записи и писем'}
            badge={<ProBadge />}
            on={form.hide_branding}
            onChange={(v) => set('hide_branding', v)}
          />
        </Group>

        <OffersGroup
          offers={form.packages}
          currency={form.currency}
          error={errors.packages}
          proLocked={proLocked && (initial.packages ?? []).length === 0}
          onChange={(packages) => set('packages', packages)}
        />

        {errors.form && (
          <div className={`notice ${errors.pro ? 'accent' : 'error'}`} role="alert">
            <span>
              {errors.form}{' '}
              {errors.pro && (
                <Link className="link" to="/settings?s=referral">
                  Пригласить коллегу
                </Link>
              )}
            </span>
          </div>
        )}
        <button className="btn btn-primary btn-lg btn-block" type="submit" disabled={busy}>
          {busy ? <Spinner small /> : page ? 'Сохранить' : 'Создать страницу'}
        </button>
      </form>
    </Sheet>
  );
}

// ------------------------------------------------------------------ пакеты (F5)

/**
  Предложения пакетов страницы: название, занятий, цена (вводится в рублях, хранится
  в копейках), срок. id сохранённых предложений не трогаем — сервер узнаёт их по id.
*/
function OffersGroup({ offers, currency, error, proLocked, onChange }: { offers: PackageOffer[]; currency: string; error?: string; proLocked: boolean; onChange: (offers: PackageOffer[]) => void }) {
  const bad = offersError(offers)?.index ?? -2;
  const update = (i: number, patch: Partial<PackageOffer>) => onChange(offers.map((o, j) => (j === i ? { ...o, ...patch } : o)));
  const foot = error ? (
    <span className="field-error">{error}</span>
  ) : proLocked ? (
    'Пакеты доступны на тарифе Pro. Пригласите коллегу — вы оба получите месяц Pro.'
  ) : offers.length ? (
    'Клиент выбирает пакет при записи и платит за него сразу; остаток и ссылка абонемента — в «Абонементах».'
  ) : (
    'Например, «8 занятий со скидкой» — клиент купит пакет при записи. Изменения уже купленных пакетов не касаются.'
  );
  return (
    <Group
      head={
        <span className="row" style={{ gap: 8 }}>
          Пакеты и абонементы <ProBadge />
        </span>
      }
      foot={foot}
    >
      {offers.map((o, i) => (
        <div key={o.id ?? `new-${i}`} className="cell stacked offer-edit" aria-label={`Пакет ${i + 1}`} role="group">
          <div className="row" style={{ gap: 8 }}>
            <input
              className={`input ${bad === i && !o.title.trim() ? 'invalid' : ''}`}
              placeholder="Название, например «8 занятий»"
              aria-label={`Название пакета ${i + 1}`}
              value={o.title}
              maxLength={OFFER_TITLE_MAX}
              onChange={(e) => update(i, { title: e.target.value })}
            />
            <button type="button" className="icon-btn" style={{ color: 'var(--danger)' }} aria-label={`Удалить пакет ${i + 1}`} onClick={() => onChange(offers.filter((_, j) => j !== i))}>
              <Trash2 size={16} />
            </button>
          </div>
          <div className="offer-grid">
            <label className="offer-field">
              <span className="small muted">Занятий</span>
              <input
                className={`input num ${bad === i && !(o.sessions >= 2 && o.sessions <= 100) ? 'invalid' : ''}`}
                type="number"
                inputMode="numeric"
                min={2}
                max={100}
                value={Number.isNaN(o.sessions) ? '' : o.sessions}
                onChange={(e) => update(i, { sessions: e.target.value === '' ? Number.NaN : Number(e.target.value) })}
              />
            </label>
            <label className="offer-field">
              <span className="small muted">Цена</span>
              <MoneyInput width="100%" label={`Цена пакета ${i + 1}`} value={o.price} currency={currency} invalid={bad === i && !(o.price > 0)} onChange={(v) => update(i, { price: v })} />
            </label>
            <label className="offer-field">
              <span className="small muted">Срок, дней</span>
              <input
                className="input num"
                type="number"
                inputMode="numeric"
                min={7}
                max={365}
                value={o.valid_days === undefined || Number.isNaN(o.valid_days) ? '' : o.valid_days}
                onChange={(e) => update(i, { valid_days: e.target.value === '' ? Number.NaN : Number(e.target.value) })}
              />
            </label>
          </div>
          {o.price > 0 && o.sessions >= 2 && (
            <span className="small muted">
              {sessionsLabel(o.sessions)} · {perSessionLabel(o.price, o.sessions, currency)}
            </span>
          )}
        </div>
      ))}
      {offers.length < OFFERS_MAX && (
        <button type="button" className="cell has-ico" onClick={() => onChange([...offers, newOffer()])}>
          <span className="ico accent">
            <Plus size={17} />
          </span>
          <span className="lab" style={{ color: 'var(--accent)' }}>
            Добавить пакет<small>{offers.length ? `ещё ${OFFERS_MAX - offers.length} из ${OFFERS_MAX}` : `до ${OFFERS_MAX} предложений`}</small>
          </span>
        </button>
      )}
    </Group>
  );
}

// ------------------------------------------------------------------ пост со свободными окнами

function PostSheet({ api, page, onClose }: { api: BookingApi; page: Page | null; onClose: () => void }) {
  const [format, setFormat] = useState<PostFormat>('telegram');
  const [days, setDays] = useState('7');
  const [text, setText] = useState('');
  const [count, setCount] = useState<number | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [copied, setCopied] = useState(false);

  useEffect(() => {
    setText('');
    setCount(null);
    setError('');
  }, [page?.id, format, days]);

  const make = async () => {
    if (!page) return;
    setBusy(true);
    setError('');
    try {
      const reply = await api.slotsPost(page.id, { days: Number(days), format });
      setText(withPostLink(reply.text, format, reply.url, publicLink(pagePath(page.slug), page.url, here())));
      setCount(reply.slots);
    } catch (e) {
      setError(bookingOwnerErrorText(e));
    } finally {
      setBusy(false);
    }
  };

  const copy = async () => {
    const ok = await copyText(text);
    setCopied(ok);
    if (ok) setTimeout(() => setCopied(false), 2000);
  };

  return (
    <Sheet open={Boolean(page)} onClose={onClose} title="Пост со свободными окнами" subtitle={page?.title}>
      <div className="stack">
        <span className="small muted">Selfvoy соберёт настоящие свободные окна этой страницы — скопируйте текст в канал, чат или сторис.</span>
        <div className="field">
          <span>Куда</span>
          <Segmented label="Формат поста" value={format} onChange={setFormat} options={POST_FORMATS} />
        </div>
        <div className="field">
          <span>На сколько дней</span>
          <Segmented
            label="Период"
            value={days}
            onChange={setDays}
            options={[
              { value: '3', label: '3 дня' },
              { value: '7', label: 'Неделя' },
              { value: '14', label: '2 недели' },
            ]}
          />
        </div>
        {!text && (
          <button type="button" className="btn btn-primary btn-lg btn-block" disabled={busy} onClick={make}>
            {busy ? <Spinner small /> : <Megaphone size={18} />} Собрать пост
          </button>
        )}
        {error && <div className="notice error">{error}</div>}
        {text && (
          <>
            <textarea className="textarea post-text" readOnly value={text} aria-label="Текст поста" onFocus={(e) => e.currentTarget.select()} />
            {count !== null && <span className="hint">Окон в посте: {count}</span>}
            <div className="grid-2">
              <button type="button" className="btn btn-primary btn-lg" onClick={copy}>
                <Copy size={17} /> {copied ? 'Скопировано' : 'Скопировать'}
              </button>
              {canShare() ? (
                <button type="button" className="btn btn-secondary btn-lg" onClick={() => navigator.share({ text }).catch(() => undefined)}>
                  <Share2 size={17} /> Поделиться
                </button>
              ) : (
                <button type="button" className="btn btn-secondary btn-lg" onClick={make} disabled={busy}>
                  {busy ? <Spinner small /> : 'Обновить'}
                </button>
              )}
            </div>
          </>
        )}
      </div>
    </Sheet>
  );
}

// ------------------------------------------------------------------ записи

function BookingRow({ booking, onOpen }: { booking: Booking; onOpen: () => void }) {
  const status = bookingStatus(booking);
  const occurrences = sortedOccurrences(booking.occurrences);
  const active = occurrences.filter((o) => o.status === 'active');
  const shown = (active.length ? active : occurrences).slice(0, 2);
  const more = (active.length ? active : occurrences).length - shown.length;
  const next = active.find((o) => Date.parse(o.start) > Date.now());
  const pay = booking.payment ? paymentLabel(booking.payment.status, 'owner') : null;

  return (
    <button type="button" className="list-row invite-row" onClick={onOpen}>
      <Avatar name={booking.guest_name || '?'} size={40} />
      <div className="body">
        <div className="ellipsis" style={{ fontWeight: 600 }}>
          {booking.guest_name}
          <span className="muted" style={{ fontWeight: 400 }}>
            {' '}
            · {booking.page_title || 'Запись'}
          </span>
        </div>
        <div className="small muted">
          {shown.map((o) => when(o.start)).join(' · ')}
          {more > 0 ? ` · ещё ${more}` : ''}
          {booking.reschedules > 0 ? ` · переносов: ${booking.reschedules}` : ''}
        </div>
        <div className="row wrap" style={{ gap: 6, marginTop: 6 }}>
          {status.label !== 'Предстоит' && <StatusPill status={{ label: status.label, tone: status.tone as 'red' | '', icon: status.tone === 'red' ? 'x' : 'check' }} />}
          {booking.status === 'active' && next?.confirmed_at && <StatusPill status={{ label: `Подтвердил(а) ${when(next.confirmed_at)}`, tone: 'green', icon: 'check' }} />}
          {booking.status === 'active' && next && !next.confirmed_at && <StatusPill status={{ label: 'Ждём «Приду»', tone: '', icon: 'clock' }} />}
          {pay && <StatusPill status={{ ...pay, label: `${pay.label} · ${formatMoney(booking.payment!.amount, booking.payment!.currency)}` }} />}
          {Boolean(booking.package_id) && <StatusPill status={{ label: 'Абонемент', tone: 'accent', icon: 'calendar' }} />}
        </div>
      </div>
      <ChevronRight size={18} className="chev" aria-hidden="true" />
    </button>
  );
}
