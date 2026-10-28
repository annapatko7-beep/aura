/*
  Деньги: сервер хранит суммы в минимальных единицах валюты (копейках, центах),
  целыми числами (docs/PROTOCOL.md → «Цена, предоплата и политика отмены»).
  Здесь — показ («2 000 ₽», «2 000,50 ₽») и разбор ввода владельца («2 000,5» → 200050).
*/
import type { Currency } from '../net/bookingApi';

export const CURRENCIES: readonly { value: Currency; label: string; symbol: string }[] = [
  { value: 'RUB', label: 'Рубли, ₽', symbol: '₽' },
  { value: 'USD', label: 'Доллары, $', symbol: '$' },
  { value: 'EUR', label: 'Евро, €', symbol: '€' },
  { value: 'KZT', label: 'Тенге, ₸', symbol: '₸' },
  { value: 'BYN', label: 'Белорусские рубли, Br', symbol: 'Br' },
];

/** Наибольшая цена страницы на сервере: 100 000 000 минимальных единиц. */
export const PRICE_MAX = 100_000_000;

const NBSP = ' ';

export function currencySymbol(currency: string | undefined): string {
  return CURRENCIES.find((c) => c.value === currency)?.symbol ?? (currency || '₽');
}

const groups = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, NBSP);

/**
  «2 000 ₽», «2 000,50 ₽», «15 $». Копейки показываются, только если они есть.
  Неверная сумма → "". Пробелы — неразрывные: сумма не переносится по строкам.
*/
export function formatMoney(minor: number | undefined | null, currency: string = 'RUB'): string {
  if (typeof minor !== 'number' || !Number.isFinite(minor)) return '';
  const negative = minor < 0;
  const abs = Math.round(Math.abs(minor));
  const whole = Math.floor(abs / 100);
  const cents = abs % 100;
  const body = groups(whole) + (cents ? `,${String(cents).padStart(2, '0')}` : '');
  return `${negative ? '−' : ''}${body}${NBSP}${currencySymbol(currency)}`;
}

/**
  Ввод суммы в рублях (долларах…) → минимальные единицы. Пустая строка → 0.
  Допускаются пробелы между разрядами и дробная часть через запятую или точку
  (до двух знаков). Мусор, минус, больше двух знаков после запятой → null.
*/
export function parseMoneyInput(text: string): number | null {
  const value = text.replace(/[\s  ]/g, '').replace(/[₽$€₸]|Br$/u, '');
  if (!value) return 0;
  const match = /^(\d+)(?:[.,](\d{0,2}))?$/.exec(value);
  if (!match) return null;
  const whole = Number(match[1]);
  const cents = Number((match[2] ?? '').padEnd(2, '0') || '0');
  const minor = whole * 100 + cents;
  return Number.isSafeInteger(minor) ? minor : null;
}

/** Сумма для поля ввода: 200000 → «2000», 200050 → «2000,50», 0 → "". */
export function moneyInputValue(minor: number | undefined): string {
  if (!minor || !Number.isFinite(minor) || minor <= 0) return '';
  const whole = Math.floor(minor / 100);
  const cents = Math.round(minor % 100);
  return cents ? `${whole},${String(cents).padStart(2, '0')}` : String(whole);
}
