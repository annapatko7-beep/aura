/*
  Календарь месяца для страницы записи гостя: сетка с понедельника, подписи,
  переход по месяцам. Дни — ключи «ГГГГ-ММ-ДД» (как dayKey из lib/invite.ts в
  поясе гостя), поэтому сетка от пояса устройства не зависит.
*/

export interface MonthCell {
  key: string;
  day: number;
}

const MONTHS = ['Январь', 'Февраль', 'Март', 'Апрель', 'Май', 'Июнь', 'Июль', 'Август', 'Сентябрь', 'Октябрь', 'Ноябрь', 'Декабрь'];
const MONTHS_SHORT_GEN = ['янв', 'фев', 'мар', 'апр', 'мая', 'июн', 'июл', 'авг', 'сен', 'окт', 'ноя', 'дек'];
export const WEEKDAYS = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс'];
const WEEKDAYS_SHORT = ['вс', 'пн', 'вт', 'ср', 'чт', 'пт', 'сб'];

const pad = (n: number) => String(n).padStart(2, '0');

/** «2026-10-22» → «2026-10». */
export const monthOf = (dayKey: string) => dayKey.slice(0, 7);

/** Месяц «ГГГГ-ММ» со сдвигом на delta месяцев. */
export function shiftMonth(month: string, delta: number): string {
  const [y, m] = month.split('-').map(Number);
  const d = new Date(Date.UTC(y, m - 1 + delta, 1));
  return `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}`;
}

/** «Октябрь 2026». */
export function monthTitle(month: string): string {
  const [y, m] = month.split('-').map(Number);
  return `${MONTHS[m - 1] ?? ''} ${y}`;
}

/** Ячейки месяца с понедельника: null — пустые клетки до первого числа. */
export function monthCells(month: string): (MonthCell | null)[] {
  const [y, m] = month.split('-').map(Number);
  const first = new Date(Date.UTC(y, m - 1, 1));
  const days = new Date(Date.UTC(y, m, 0)).getUTCDate();
  const lead = (first.getUTCDay() + 6) % 7;
  const cells: (MonthCell | null)[] = Array.from({ length: lead }, () => null);
  for (let d = 1; d <= days; d += 1) cells.push({ key: `${y}-${pad(m)}-${pad(d)}`, day: d });
  return cells;
}

/** «чт, 22 окт» для ключа дня (день недели — по календарю, без пояса). */
export function shortDay(dayKey: string): string {
  const [y, m, d] = dayKey.split('-').map(Number);
  const date = new Date(Date.UTC(y, m - 1, d));
  return `${WEEKDAYS_SHORT[date.getUTCDay()]}, ${d} ${MONTHS_SHORT_GEN[m - 1] ?? ''}`;
}
