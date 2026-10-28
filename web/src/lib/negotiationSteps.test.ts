import { describe, expect, it } from 'vitest';
import { selfvoyOf, negotiationSteps } from './negotiationSteps';

const now = new Date('2026-10-13T09:00:00');
const base = { targetName: 'Дима Петров', topic: 'обсудить стартап', durationMinutes: 60, windowHours: 168 };

describe('negotiationSteps', () => {
  it('договорились: предложение → календарь → место → итог', () => {
    const steps = negotiationSteps(
      {
        ...base,
        result: {
          accepted: true,
          slot: { start: '2026-10-14T10:00:00', end: '2026-10-14T11:00:00' },
          place: { name: 'Aurora Roasters', city: 'Керкраде', rating: 4.6, tags: ['coffee'] },
          message: 'Встреча завтра в 10:00',
        },
      },
      now,
    );
    expect(steps.map((s) => s.state)).toEqual(['info', 'info', 'info', 'ok']);
    expect(steps[0].text).toContain('60 мин');
    expect(steps[1].actor).toBe(selfvoyOf('Дима Петров'));
    expect(steps[2].text).toContain('Aurora Roasters');
    expect(steps[3].detail).toBe('Встреча завтра в 10:00');
  });

  it('без места (приватность запрещает выбор локаций)', () => {
    const steps = negotiationSteps({ ...base, result: { accepted: true, slot: { start: '2026-10-14T10:00:00', end: '2026-10-14T11:00:00' } } }, now);
    expect(steps[2].text).toBe('Место не выбрано');
    expect(steps.at(-1)?.state).toBe('ok');
  });

  it('нет общего окна: встречные варианты или отказ', () => {
    const withCounter = negotiationSteps(
      { ...base, result: { accepted: false, counter_slots: [{ start: '2026-10-20T12:00:00', end: '2026-10-20T13:00:00' }] } },
      now,
    );
    expect(withCounter[1].state).toBe('fail');
    expect(withCounter[2].text).toBe('Предложил другое время');

    const none = negotiationSteps({ ...base, result: { accepted: false } }, now);
    expect(none.at(-1)).toMatchObject({ actor: 'Итог', state: 'fail' });
  });

  it('email вместо имени не склоняется', () => {
    expect(selfvoyOf('dima@selfvoy.local')).toBe('Selfvoy dima@selfvoy.local');
  });
});
