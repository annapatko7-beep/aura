/*
  Подключения и тариф: ссылка подписки на календарь (F6), подпись тарифа (F3),
  ошибка «нужен Pro». Без React — тестируется в integrations.test.ts.
*/
import type { Plan } from '../net/api';
import { SelfvoyError } from '../net/socket';
import { dateLabel } from './time';

/**
  Адрес подписки на календарь. Готовый `url` от сервера (SELFVOY_SERVER_PUBLIC_URL) —
  как есть; иначе собираем из адреса сервера, к которому подключено приложение:
  wss://host → https://host, ws://host → http://host (тот же порт, путь `path`).
  Нечего собрать → null.
*/
export function icsFeedUrl(url: string | undefined, path: string | undefined, serverUrl: string | undefined): string | null {
  const ready = (url ?? '').trim();
  if (/^https?:\/\//i.test(ready)) return ready;
  const tail = (path ?? '').trim();
  if (!tail.startsWith('/')) return null;
  try {
    const server = new URL((serverUrl ?? '').trim());
    const scheme = server.protocol === 'wss:' ? 'https:' : server.protocol === 'ws:' ? 'http:' : '';
    if (!scheme || !server.host) return null;
    return `${scheme}//${server.host}${tail}`;
  } catch {
    return null;
  }
}

/** webcal:// — календари на телефоне открывают подписку сразу; только для https. */
export function webcalUrl(url: string | null): string | null {
  return url && /^https:\/\//i.test(url) ? url.replace(/^https:/i, 'webcal:') : null;
}

export interface PlanView {
  label: string;
  pro: boolean;
  /** Пояснение под подписью (бета, срок). */
  hint: string;
}

/** «Pro до 12 ноября» / «Free». Без поля plan (старый сервер) — null. */
export function planView(plan: Plan | undefined, now = new Date()): PlanView | null {
  if (!plan) return null;
  const until = plan.until ? new Date(plan.until) : null;
  const pro = plan.name === 'pro' && (!until || Number.isNaN(until.getTime()) || until.getTime() > now.getTime());
  if (pro) {
    const label = until && !Number.isNaN(until.getTime()) ? `Pro до ${dateLabel(until)}` : 'Pro';
    return { label, pro: true, hint: 'Скрытая подпись, предоплата и ЮKassa доступны' };
  }
  return {
    label: 'Free',
    pro: false,
    hint: plan.enforced ? 'Функции Pro: скрыть подпись, предоплата, ЮKassa' : 'Пока идёт бета, функции Pro открыты всем',
  };
}

/** Действие требует Pro (код pro_required). */
export const isProRequired = (error: unknown) => error instanceof SelfvoyError && error.code === 'pro_required';

export const PRO_REQUIRED_TEXT = 'Это доступно на тарифе Pro. Пригласите коллегу в Selfvoy — вы оба получите месяц Pro (Настройки → Пригласить друзей).';
