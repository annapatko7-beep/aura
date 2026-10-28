/*
  Таймлайн переговоров Аур из ответа agent.negotiate (accepted, slot, place,
  counter_slots, message): короткие понятные шаги вместо схемы.
*/
import type { NegotiationResult } from '../net/api';
import { firstName, genitive, placeTags, when, windowPhrase } from './format';

export interface Step {
  actor: string;
  text: string;
  detail?: string;
  state: 'ok' | 'fail' | 'info';
}

export interface NegotiationInput {
  targetName: string;
  topic: string;
  durationMinutes: number;
  windowHours: number;
  result: NegotiationResult;
}

export function selfvoyOf(name: string): string {
  const first = firstName(name);
  return first.includes('@') ? `Selfvoy ${first}` : `Selfvoy ${genitive(first)}`;
}

export function negotiationSteps(n: NegotiationInput, now = new Date()): Step[] {
  const them = selfvoyOf(n.targetName);
  const r = n.result;
  const steps: Step[] = [
    {
      actor: 'Ваш Selfvoy',
      text: `Предложил встречу на ${n.durationMinutes} мин`,
      detail: `Окно — ${windowPhrase(n.windowHours)}${n.topic ? ` · ${n.topic}` : ''}`,
      state: 'info',
    },
  ];
  if (r.accepted && r.slot) {
    steps.push({ actor: them, text: 'Сверил календарь', detail: `Общее окно: ${when(r.slot.start, now)}`, state: 'info' });
    if (r.place?.name) {
      const facts = [r.place.city, placeTags(r.place.tags), r.place.rating ? `★ ${r.place.rating}` : ''].filter(Boolean);
      steps.push({ actor: them, text: `Предложил место — ${r.place.name}`, detail: facts.join(' · '), state: 'info' });
    } else {
      steps.push({ actor: them, text: 'Место не выбрано', detail: 'Выбор места выключен в настройках приватности — договоритесь сами', state: 'info' });
    }
    steps.push({ actor: 'Итог', text: 'Договорились', detail: r.message, state: 'ok' });
  } else {
    steps.push({ actor: them, text: 'Сверил календарь', detail: 'В этом окне общего времени нет', state: 'fail' });
    const counter = (r.counter_slots ?? []).slice(0, 3).map((s) => when(s.start, now));
    steps.push(
      counter.length
        ? { actor: them, text: 'Предложил другое время', detail: counter.join(' · '), state: 'info' }
        : { actor: 'Итог', text: 'Не договорились', detail: 'Попробуйте окно пошире', state: 'fail' },
    );
  }
  return steps;
}
