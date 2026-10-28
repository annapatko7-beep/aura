import { describe, expect, it } from 'vitest';
import { monthCells, monthOf, monthTitle, shiftMonth, shortDay } from './monthCalendar';

describe('календарь месяца', () => {
  it('октябрь 2026 начинается с четверга: три пустые клетки', () => {
    const cells = monthCells('2026-10');
    expect(cells.slice(0, 4)).toEqual([null, null, null, { key: '2026-10-01', day: 1 }]);
    expect(cells.filter(Boolean)).toHaveLength(31);
    expect(cells.at(-1)).toEqual({ key: '2026-10-31', day: 31 });
  });

  it('месяц с понедельника — без пустых клеток, февраль невисокосного года — 28 дней', () => {
    expect(monthCells('2026-06')[0]).toEqual({ key: '2026-06-01', day: 1 });
    expect(monthCells('2026-02').filter(Boolean)).toHaveLength(28);
    expect(monthCells('2028-02').filter(Boolean)).toHaveLength(29);
  });

  it('переход по месяцам и подписи', () => {
    expect(shiftMonth('2026-12', 1)).toBe('2027-01');
    expect(shiftMonth('2026-01', -1)).toBe('2025-12');
    expect(monthTitle('2026-10')).toBe('Октябрь 2026');
    expect(monthOf('2026-10-22')).toBe('2026-10');
    expect(shortDay('2026-10-22')).toBe('чт, 22 окт');
  });
});
