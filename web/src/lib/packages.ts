/*
  Пакеты занятий и абонементы (F5): чистая логика без React (тестируется в
  packages.test.ts) — подписи статусов, остаток и срок, проверка предложений
  страницы и формы выдачи, второй получатель (cc_email), тексты уведомлений.
*/
import type { PackageOffer, PackageStatus } from '../net/packageApi';
import { plural } from './format';
import { EMAIL_MAX, GUEST_NAME_MAX } from './invite';
import { formatMoney, PRICE_MAX } from './money';
import type { StatusView } from './payment';

// ------------------------------------------------------------------ ссылки

export const packagePath = (token: string) => `/p/${token}`;

/** Запись по абонементу: страница записи с токеном пакета (?pkg=<токен>). */
export const packageBookPath = (slug: string, token: string) => `/b/${slug}?pkg=${encodeURIComponent(token)}`;

/** Токен пакета из адреса страницы записи: 43 символа base64url (как выдаёт сервер); иначе null. */
export function packageTokenFrom(search: string | URLSearchParams): string | null {
  const params = typeof search === 'string' ? new URLSearchParams(search.startsWith('?') ? search.slice(1) : search) : search;
  const token = (params.get('pkg') ?? '').trim();
  return /^[A-Za-z0-9_-]{20,64}$/.test(token) ? token : null;
}

/**
  Подпись строки «Оплата · По абонементу» в карточке записи владельца. Списано —
  пока встреча в силе; отмена владельцем возвращает занятие всегда, отмена гостем —
  только не позже срока бесплатной отмены (поздняя — нет), поэтому про гостевую
  отмену без точного остатка ничего не утверждаем.
*/
export function packageSessionNote(
  booking: { status: string; cancelled_by?: string },
  occurrence: { status: string } | undefined,
): string {
  const cancelled = booking.status === 'cancelled' || occurrence?.status === 'cancelled';
  if (!cancelled) return 'занятие списано с абонемента клиента';
  if (booking.status === 'cancelled' && booking.cancelled_by === 'owner') return 'вы отменили встречу — занятие вернулось на абонемент';
  return 'встреча отменена — остаток занятий в «Абонементах»';
}

// ------------------------------------------------------------------ подписи

const STATUS: Record<PackageStatus, { owner: string; guest: string; tone: StatusView['tone']; icon: StatusView['icon'] }> = {
  pending: { owner: 'Ждёт оплаты', guest: 'Ждём оплату', tone: 'amber', icon: 'clock' },
  active: { owner: 'Действует', guest: 'Действует', tone: 'green', icon: 'check' },
  used: { owner: 'Занятия закончились', guest: 'Занятия закончились', tone: '', icon: 'minus' },
  expired: { owner: 'Срок истёк', guest: 'Срок истёк', tone: 'red', icon: 'x' },
  cancelled: { owner: 'Отменён', guest: 'Отменён', tone: 'red', icon: 'x' },
};

/** Статус пакета — значок и слово (не только цвет). */
export function packageStatusView(status: PackageStatus | string | undefined, who: 'owner' | 'guest' = 'owner'): StatusView {
  const view = STATUS[status as PackageStatus];
  if (!view) return { label: status ? String(status) : 'Пакет', tone: '', icon: 'minus' };
  return { label: who === 'owner' ? view.owner : view.guest, tone: view.tone, icon: view.icon };
}

/** Фильтр списка абонементов владельца. */
export const PACKAGE_FILTERS: readonly { value: '' | PackageStatus; label: string }[] = [
  { value: '', label: 'Все' },
  { value: 'active', label: 'Действуют' },
  { value: 'pending', label: 'Ждут оплаты' },
  { value: 'used', label: 'Закончились' },
  { value: 'expired', label: 'Истекли' },
  { value: 'cancelled', label: 'Отменены' },
];

/** «8 занятий». */
export const sessionsLabel = (n: number) => `${n} ${plural(n, 'занятие', 'занятия', 'занятий')}`;

/** «1 500 ₽ за занятие» (с округлением до рубля, если копеек нет). */
export function perSessionLabel(price: number, sessions: number, currency = 'RUB'): string {
  if (!price || !sessions) return '';
  return `${formatMoney(Math.round(price / sessions), currency)} за занятие`;
}

/** «8 занятий · 12 000 ₽ · 90 дней». */
export function offerSummary(offer: Pick<PackageOffer, 'sessions' | 'price' | 'valid_days'>, currency = 'RUB'): string {
  const parts = [sessionsLabel(offer.sessions)];
  if (offer.price) parts.push(formatMoney(offer.price, currency));
  const days = offer.valid_days ?? OFFER_DEFAULT_DAYS;
  parts.push(`${days} ${plural(days, 'день', 'дня', 'дней')}`);
  return parts.join(' · ');
}

/** Остаток и прогресс: used/total, доля использованного (0..1). */
export function packageProgress(pkg: { sessions_total: number; sessions_left: number; sessions_used?: number }): { used: number; total: number; left: number; ratio: number } {
  const total = Math.max(0, Math.round(pkg.sessions_total || 0));
  const left = Math.min(total, Math.max(0, Math.round(pkg.sessions_left || 0)));
  const used = typeof pkg.sessions_used === 'number' ? Math.min(total, Math.max(0, Math.round(pkg.sessions_used))) : total - left;
  return { used, total, left, ratio: total > 0 ? used / total : 0 };
}

/** «осталось 3 из 8». */
export function leftLabel(pkg: { sessions_total: number; sessions_left: number }): string {
  const { left, total } = packageProgress(pkg);
  return `осталось ${left} из ${total}`;
}

/** Дуга кольца остатка: длина окружности и смещение (stroke-dasharray / dashoffset). */
export function ringDash(left: number, total: number, radius: number): { length: number; offset: number } {
  const length = 2 * Math.PI * radius;
  const share = total > 0 ? Math.min(1, Math.max(0, left / total)) : 0;
  return { length, offset: length * (1 - share) };
}

/**
  Срок действия: "" — ещё не оплачен (срок пойдёт с момента оплаты); иначе «Срок — до 20 января 2027 г.
  · ещё 12 дней» / «истёк 3 янв.».
*/
export function validityLabel(pkg: { valid_until: string; valid_days: number; status?: PackageStatus | string }, now = new Date()): string {
  const until = pkg.valid_until ? new Date(pkg.valid_until) : null;
  if (!until || Number.isNaN(until.getTime())) {
    return pkg.valid_days ? `${pkg.valid_days} ${plural(pkg.valid_days, 'день', 'дня', 'дней')} с момента оплаты` : 'Срок начнётся после оплаты';
  }
  const date = until.toLocaleDateString('ru-RU', { day: 'numeric', month: 'long', year: until.getFullYear() === now.getFullYear() ? undefined : 'numeric' });
  const ms = until.getTime() - now.getTime();
  if (ms <= 0 || pkg.status === 'expired') return `Срок истёк ${date}`;
  const days = Math.ceil(ms / 86_400_000);
  return `Срок — до ${date} · ещё ${days} ${plural(days, 'день', 'дня', 'дней')}`;
}

/** Можно ли записаться по пакету: действует, есть занятия, страница открыта. */
export function canBookByPackage(pkg: { status: PackageStatus | string; sessions_left: number; page_slug: string; valid_until?: string }, now = new Date()): boolean {
  if (pkg.status !== 'active' || pkg.sessions_left <= 0 || !pkg.page_slug) return false;
  if (pkg.valid_until && Date.parse(pkg.valid_until) <= now.getTime()) return false;
  return true;
}

/** Почему по пакету сейчас не записаться (для гостя); "" — можно. */
export function packageBlockReason(pkg: { status: PackageStatus | string; sessions_left: number; page_slug: string; valid_until?: string }, now = new Date()): string {
  if (pkg.status === 'pending') return 'Записаться по абонементу можно после оплаты.';
  if (pkg.status === 'cancelled') return 'Абонемент отменён.';
  if (pkg.status === 'expired' || (pkg.valid_until && Date.parse(pkg.valid_until) <= now.getTime())) return 'Срок действия абонемента истёк.';
  if (pkg.status === 'used' || pkg.sessions_left <= 0) return 'Занятия по абонементу закончились — можно купить новый.';
  if (!pkg.page_slug) return 'Запись сейчас закрыта — напишите специалисту.';
  return '';
}

// ------------------------------------------------------------------ предложения страницы

export const OFFERS_MAX = 5;
export const OFFER_TITLE_MAX = 80;
export const OFFER_DEFAULT_DAYS = 90;

const length = (s: string) => [...s].length;
const inRange = (n: number, min: number, max: number) => Number.isInteger(n) && n >= min && n <= max;

/** Новое предложение в форме: id нет — сервер выдаст. */
export const newOffer = (): PackageOffer => ({ title: '', sessions: 8, price: 0, valid_days: OFFER_DEFAULT_DAYS });

/**
  Пределы — как на сервере: до 5 предложений, название 1–80 в одну строку,
  2–100 занятий, цена > 0 и ≤ 100 000 000 мин. единиц, срок 7–365 дней.
  Ответ — текст первой ошибки и номер предложения (−1 — про весь список).
*/
export function offersError(offers: PackageOffer[] | undefined): { index: number; text: string } | null {
  const list = offers ?? [];
  if (list.length > OFFERS_MAX) return { index: -1, text: `Не больше ${OFFERS_MAX} пакетов на странице` };
  for (let i = 0; i < list.length; i += 1) {
    const o = list[i];
    const title = (o.title ?? '').trim();
    const n = `Пакет ${i + 1}`;
    if (!title) return { index: i, text: `${n}: назовите его — например, «Абонемент на 8 занятий»` };
    if (length(title) > OFFER_TITLE_MAX || /[\r\n]/.test(title)) return { index: i, text: `${n}: название — до ${OFFER_TITLE_MAX} символов в одну строку` };
    if (!inRange(o.sessions, 2, 100)) return { index: i, text: `${n}: занятий — от 2 до 100` };
    if (!inRange(o.price, 1, PRICE_MAX)) return { index: i, text: `${n}: укажите цену больше нуля, не больше 1 000 000` };
    if (!inRange(o.valid_days ?? OFFER_DEFAULT_DAYS, 7, 365)) return { index: i, text: `${n}: срок действия — от 7 до 365 дней` };
  }
  return null;
}

/** Предложения для сервера: названия без пробелов по краям, id — только у сохранённых. */
export function cleanOffers(offers: PackageOffer[] | undefined): PackageOffer[] {
  return (offers ?? []).map((o) => {
    const out: PackageOffer = { title: o.title.trim(), sessions: o.sessions, price: o.price, valid_days: o.valid_days ?? OFFER_DEFAULT_DAYS };
    return typeof o.id === 'number' && o.id > 0 ? { id: o.id, ...out } : out;
  });
}

/** Совпадают ли списки предложений (pageUpdate шлёт packages, только если изменились). */
export function offersEqual(a: PackageOffer[] | undefined, b: PackageOffer[] | undefined): boolean {
  const x = cleanOffers(a);
  const y = cleanOffers(b);
  return x.length === y.length && x.every((o, i) => o.id === y[i].id && o.title === y[i].title && o.sessions === y[i].sessions && o.price === y[i].price && o.valid_days === y[i].valid_days);
}

// ------------------------------------------------------------------ второй получатель

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/;

/** Копия напоминаний: необязательно; формат email и не тот же адрес, что у гостя (без учёта регистра). */
export function ccEmailError(cc: string, guestEmail: string): string | null {
  const value = cc.trim();
  if (!value) return null;
  if (length(value) > EMAIL_MAX || !EMAIL_RE.test(value)) return 'Похоже, в email опечатка';
  if (value.toLowerCase() === guestEmail.trim().toLowerCase()) return 'Это ваш же адрес — укажите другой или оставьте поле пустым';
  return null;
}

// ------------------------------------------------------------------ выдача владельцем

export interface IssueForm {
  page_id: number;
  /** 0 — своё название и число занятий. */
  offer_id: number;
  title: string;
  sessions: number;
  price: number;
  valid_days: number;
  guest_name: string;
  guest_email: string;
  cc_email: string;
  paid: boolean;
}

export interface IssueErrors {
  page?: string;
  offer?: string;
  guest_name?: string;
  guest_email?: string;
  cc_email?: string;
}

/** Форма package.issue: пределы как на сервере (своё предложение — 1–100 занятий, цена ≥ 0). */
export function issueErrors(form: IssueForm): IssueErrors {
  const errors: IssueErrors = {};
  if (!form.page_id) errors.page = 'Выберите страницу записи';
  if (!form.offer_id) {
    const title = form.title.trim();
    if (!title || length(title) > OFFER_TITLE_MAX || /[\r\n]/.test(title)) errors.offer = `Название — от 1 до ${OFFER_TITLE_MAX} символов в одну строку`;
    else if (!inRange(form.sessions, 1, 100)) errors.offer = 'Занятий — от 1 до 100';
    else if (!inRange(form.price, 0, PRICE_MAX)) errors.offer = 'Проверьте цену: например, 12000 или 0';
    else if (!inRange(form.valid_days, 7, 365)) errors.offer = 'Срок действия — от 7 до 365 дней';
  }
  const name = form.guest_name.trim();
  if (!name) errors.guest_name = 'Как зовут клиента?';
  else if (length(name) > GUEST_NAME_MAX) errors.guest_name = `Имя — до ${GUEST_NAME_MAX} символов`;
  const email = form.guest_email.trim();
  if (email && (length(email) > EMAIL_MAX || !EMAIL_RE.test(email))) errors.guest_email = 'Похоже, в email опечатка';
  const cc = ccEmailError(form.cc_email, form.guest_email);
  if (cc) errors.cc_email = cc;
  return errors;
}

// ------------------------------------------------------------------ уведомления

/** Уведомления о пакетах — для «Входящих»: короткая строка из payload, если сервер не прислал текст. */
export function packageNoticeDetail(kind: string, payload: Record<string, unknown> | undefined): string {
  const p = payload ?? {};
  const name = typeof p.guest_name === 'string' ? p.guest_name : '';
  const who = name ? `${name}: ` : '';
  if (kind === 'package.purchased') return `${who}купил(а) пакет при записи`;
  if (kind === 'package.low') {
    const left = Number(p.left ?? 0);
    return left > 0 ? `${who}осталось ${sessionsLabel(left)} — предложите продлить` : `${who}занятия по абонементу закончились`;
  }
  if (kind === 'package.payment') {
    const amount = typeof p.amount === 'number' ? formatMoney(p.amount, typeof p.currency === 'string' ? p.currency : 'RUB') : '';
    const sum = amount ? ` ${amount}` : '';
    switch (p.status) {
      case 'claimed':
        return `${who}отметил(а) оплату${sum} — проверьте поступление`;
      case 'paid':
        return `${who}оплата${sum} получена`;
      case 'refunded':
        return `${who}деньги${sum} возвращены`;
      case 'refund_due':
        return `${who}верните оплату${sum} вручную`;
      case 'expired':
        return `${who}оплата не поступила — покупка отменена`;
    }
  }
  return '';
}

export const isPackageNotice = (kind: string) => kind.startsWith('package.');
