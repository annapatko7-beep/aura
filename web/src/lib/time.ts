const DAY_NAMES = ['воскресенье', 'понедельник', 'вторник', 'среда', 'четверг', 'пятница', 'суббота'];
const DAY_ACC = ['воскресенье', 'понедельник', 'вторник', 'среду', 'четверг', 'пятницу', 'субботу'];
const DAY_SHORT = ['Вс', 'Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб'];
const MONTHS_GEN = ['января', 'февраля', 'марта', 'апреля', 'мая', 'июня', 'июля', 'августа', 'сентября', 'октября', 'ноября', 'декабря'];

export const pad = (n: number) => String(n).padStart(2, '0');

export function hhmm(d: Date): string {
  return `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

export function hourOf(d: Date): number {
  return d.getHours() + d.getMinutes() / 60;
}

export function startOfDay(d: Date): Date {
  const x = new Date(d);
  x.setHours(0, 0, 0, 0);
  return x;
}

export function addDays(d: Date, n: number): Date {
  const x = new Date(d);
  x.setDate(x.getDate() + n);
  return x;
}

export function sameDay(a: Date, b: Date): boolean {
  return a.getFullYear() === b.getFullYear() && a.getMonth() === b.getMonth() && a.getDate() === b.getDate();
}

/** "сегодня", "завтра" or "в субботу" — for sentences like "предложил субботу в 13:00". */
export function dayPhrase(d: Date, now: Date, accusative = true): string {
  if (sameDay(d, now)) return 'сегодня';
  if (sameDay(d, addDays(now, 1))) return 'завтра';
  return (accusative ? DAY_ACC : DAY_NAMES)[d.getDay()];
}

export function dayTitle(d: Date): string {
  const name = DAY_NAMES[d.getDay()];
  return name[0].toUpperCase() + name.slice(1);
}

export function dayShort(d: Date): string {
  return DAY_SHORT[d.getDay()];
}

export function dateLabel(d: Date): string {
  return `${d.getDate()} ${MONTHS_GEN[d.getMonth()]}`;
}

export function greeting(d: Date): string {
  const h = d.getHours();
  if (h < 5) return 'Доброй ночи';
  if (h < 12) return 'Доброе утро';
  if (h < 18) return 'Добрый день';
  return 'Добрый вечер';
}

export function rub(n: number): string {
  return `${n.toLocaleString('ru-RU')} ₽`;
}

/** Monday-first week containing d. */
export function weekOf(d: Date): Date[] {
  const start = startOfDay(d);
  const shift = (start.getDay() + 6) % 7;
  const monday = addDays(start, -shift);
  return Array.from({ length: 7 }, (_, i) => addDays(monday, i));
}
