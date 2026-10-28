/*
  Форматирование для интерфейса: даты по-русски, подписи инструментов Selfvoy,
  теги мест. Те же формулировки, что в нативных клиентах (Format.kt/.swift/.js).
*/
import { addDays, dateLabel, dayShort, hhmm, sameDay } from './time';

export function parseDate(iso?: string | null): Date | null {
  if (!iso) return null;
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? null : d;
}

/** «сегодня, 14:00», «завтра, 10:00», «вчера, 18:05», «ср, 15 октября, 12:00». */
export function when(iso?: string | null, now = new Date()): string {
  const d = parseDate(iso);
  if (!d) return '';
  const time = hhmm(d);
  if (sameDay(d, now)) return `сегодня, ${time}`;
  if (sameDay(d, addDays(now, 1))) return `завтра, ${time}`;
  if (sameDay(d, addDays(now, -1))) return `вчера, ${time}`;
  return `${dayShort(d).toLowerCase()}, ${dateLabel(d)}, ${time}`;
}

/** Только время для сегодняшних событий, иначе дата: для плотных списков. */
export function shortWhen(iso?: string | null, now = new Date()): string {
  const d = parseDate(iso);
  if (!d) return '';
  if (sameDay(d, now)) return hhmm(d);
  if (sameDay(d, addDays(now, 1))) return `завтра ${hhmm(d)}`;
  return `${dateLabel(d)}, ${hhmm(d)}`;
}

const TOOL_TITLES: Record<string, string> = {
  send_message: 'Сообщения',
  create_note: 'Заметки',
  create_reminder: 'Напоминания',
  send_email: 'Письма',
  find_cafe: 'Подбор мест',
  book_table: 'Бронирование',
  check_calendar: 'Календарь',
  suggest_time: 'Подбор времени',
};

const TOOL_DESCRIPTIONS: Record<string, string> = {
  send_message: 'Писать сообщения вашим контактам',
  create_note: 'Сохранять заметки в память Selfvoy',
  create_reminder: 'Ставить напоминания',
  send_email: 'Отправлять письма от вашего имени',
  find_cafe: 'Подбирать место под питание, бюджет и дорогу обоих',
  book_table: 'Бронировать столик в выбранном месте',
  check_calendar: 'Смотреть, свободны ли вы в нужное время',
  suggest_time: 'Предлагать время встречи',
};

export const toolTitle = (tool: string) => TOOL_TITLES[tool] ?? tool;
export const toolDescription = (tool: string, fallback = '') => TOOL_DESCRIPTIONS[tool] ?? fallback;

const PLACE_TAGS: Record<string, string> = {
  coffee: 'кофе',
  brunch: 'бранчи',
  dinner: 'ужины',
  vegan: 'веганское меню',
  gluten_free: 'без глютена',
  meat: 'мясо',
  quiet: 'тихо',
  workspace: 'можно поработать',
};

export const placeTags = (tags: string[] = []) => tags.map((t) => PLACE_TAGS[t] ?? t.replace(/_/g, ' ')).join(', ');

/** Окно переговоров в часах: «сегодня–завтра», «до конца недели», «две недели». */
export const WINDOWS = [
  { hours: 48, label: 'Сегодня–завтра' },
  { hours: 168, label: 'Неделя' },
  { hours: 336, label: 'Две недели' },
] as const;

export function windowPhrase(hours: number): string {
  if (hours < 48) return `ближайшие ${hours} ч`;
  const days = Math.round(hours / 24);
  return `ближайшие ${days} ${plural(days, 'день', 'дня', 'дней')}`;
}

export function plural(n: number, one: string, few: string, many: string): string {
  const m10 = n % 10;
  const m100 = n % 100;
  if (m10 === 1 && m100 !== 11) return one;
  if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few;
  return many;
}

export const DIETS = [
  { value: 'vegan', label: 'Веган' },
  { value: 'vegetarian', label: 'Вегетарианец' },
  { value: 'gluten_free', label: 'Без глютена' },
  { value: 'lactose_free', label: 'Без лактозы' },
  { value: 'halal', label: 'Халяль' },
  { value: 'kosher', label: 'Кошер' },
];

export function firstName(full: string): string {
  return full.trim().split(/\s+/)[0] ?? '';
}

/** Родительный падеж имени для «Selfvoy Димы», «Selfvoy Алекса» — простые правила для частых имён. */
export function genitive(name: string): string {
  const n = name.trim();
  if (!n || n.includes('@')) return n;
  const last = n.slice(-1).toLowerCase();
  const prev = n.slice(-2, -1).toLowerCase();
  if (last === 'я') return n.slice(0, -1) + 'и';
  if (last === 'а') return n.slice(0, -1) + ('гкхжчшщ'.includes(prev) ? 'и' : 'ы');
  if (last === 'й' || last === 'ь') return n.slice(0, -1) + 'я';
  if ('бвгджзклмнпрстфхцчшщ'.includes(last)) return n + 'а';
  return n;
}
