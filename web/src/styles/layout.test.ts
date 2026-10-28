/*
  Вёрстка, которую ломали на живом стенде (узкий экран 360–390 px):

  1. Одноколоночные сетки списков и групп (.list, .group, .cell.stacked) без явной
     колонки растягивали неявную auto-колонку по min-content строки в nowrap
     (ссылка страницы, .ellipsis): строки задач на «Сегодня» уезжали за экран, у
     карточки страницы записи пропадала кнопка «Скопировать».
  2. .notice — flex-контейнер: «текст <span>адрес</span> текст» внутри него
     разъезжался на три колонки (ссылка управления записью у гостя, код в режиме
     разработки на входе). Смешанный текст оборачивается в один <span>.
*/
/// <reference types="node" />
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

// Не через «?raw»: vitest подменяет содержимое .css пустой строкой.
const css = readFileSync(new URL('./global.css', import.meta.url), 'utf8');

const sources = import.meta.glob('../**/*.tsx', { query: '?raw', import: 'default', eager: true }) as Record<string, string>;

/** Тело CSS-правила с точным селектором (первое вхождение). */
function rule(selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const match = new RegExp(`(^|\\n)${escaped}\\s*\\{([^}]*)\\}`).exec(css);
  return match ? match[2] : '';
}

describe('одноколоночные сетки не шире контейнера', () => {
  for (const selector of ['.list', '.group', '.cell.stacked']) {
    it(selector, () => {
      const body = rule(selector);
      expect(body).toContain('display: grid');
      expect(body).toMatch(/grid-template-columns:\s*minmax\(0,\s*1fr\)/);
    });
  }
});

/** Содержимое <div className="notice…">…</div> (простой разбор по глубине тегов div). */
function notices(source: string): { line: number; inner: string }[] {
  const out: { line: number; inner: string }[] = [];
  const open = /<div\s+className=\{?[`"]notice\b[^>]*>/g;
  let m: RegExpExecArray | null;
  while ((m = open.exec(source))) {
    let depth = 1;
    let i = m.index + m[0].length;
    const tag = /<div\b[^>]*?(\/?)>|<\/div>/g;
    tag.lastIndex = i;
    let t: RegExpExecArray | null;
    while (depth > 0 && (t = tag.exec(source))) {
      if (t[0] === '</div>') depth -= 1;
      else if (t[1] !== '/') depth += 1;
      i = t.index;
    }
    out.push({ line: source.slice(0, m.index).split('\n').length, inner: source.slice(m.index + m[0].length, i) });
  }
  return out;
}

/** Текст вперемешку со строчными элементами прямо в .notice (иконка в начале не считается). */
function mixedInline(inner: string): boolean {
  const body = inner
    .replace(/\{\/\*[\s\S]*?\*\/\}/g, '') // JSX-комментарии
    .replace(/^\s*<[A-Z]\w*[^>]*\/>/, ''); // ведущая иконка lucide
  // Вложенные элементы целиком убираем — смотрим только на прямых детей.
  let direct = body;
  for (let k = 0; k < 10; k++) direct = direct.replace(/<(\w+)\b[^>]*>(?:(?!<\1\b)[\s\S])*?<\/\1>|<\w+\b[^>]*\/>/g, '\u0000');
  const hasElement = direct.includes('\u0000');
  const text = direct.replace(/\u0000/g, '').replace(/\{[^{}]*\}/g, '').trim();
  return hasElement && text.length > 0;
}

describe('.notice: смешанный текст обёрнут в один элемент', () => {
  it('разбор видит смешанное содержимое', () => {
    expect(mixedInline('Код: <b className="mono">{code}</b>')).toBe(true);
    expect(mixedInline('<span>Код: <b className="mono">{code}</b></span>')).toBe(false);
    expect(mixedInline('<Check size={16} /> Вы записаны')).toBe(false);
    expect(mixedInline('{error}')).toBe(false);
  });

  it('во всех .tsx', () => {
    const bad: string[] = [];
    for (const [file, source] of Object.entries(sources)) {
      for (const n of notices(source)) if (mixedInline(n.inner)) bad.push(`${file}:${n.line}`);
    }
    expect(bad).toEqual([]);
  });
});
