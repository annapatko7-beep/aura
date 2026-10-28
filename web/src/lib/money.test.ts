import { describe, expect, it } from 'vitest';
import { currencySymbol, formatMoney, moneyInputValue, parseMoneyInput } from './money';

const s = (text: string) => text.replace(/ /g, ' ');

describe('formatMoney', () => {
  it('минимальные единицы → сумма с символом валюты', () => {
    expect(s(formatMoney(200000))).toBe('2 000 ₽');
    expect(s(formatMoney(250000, 'RUB'))).toBe('2 500 ₽');
    expect(s(formatMoney(200050, 'RUB'))).toBe('2 000,50 ₽');
    expect(s(formatMoney(5, 'RUB'))).toBe('0,05 ₽');
    expect(s(formatMoney(1500, 'USD'))).toBe('15 $');
    expect(s(formatMoney(99, 'EUR'))).toBe('0,99 €');
    expect(s(formatMoney(500000000, 'KZT'))).toBe('5 000 000 ₸');
    expect(s(formatMoney(3000, 'BYN'))).toBe('30 Br');
    expect(s(formatMoney(0))).toBe('0 ₽');
  });

  it('пробелы неразрывные, мусор → пусто', () => {
    expect(formatMoney(200000)).toBe('2 000 ₽');
    expect(formatMoney(Number.NaN)).toBe('');
    expect(formatMoney(undefined)).toBe('');
    expect(formatMoney(null)).toBe('');
  });

  it('неизвестная валюта — её код', () => {
    expect(currencySymbol('GBP')).toBe('GBP');
    expect(currencySymbol(undefined)).toBe('₽');
  });
});

describe('parseMoneyInput', () => {
  it('рубли с копейками → копейки', () => {
    expect(parseMoneyInput('2000')).toBe(200000);
    expect(parseMoneyInput('2 000')).toBe(200000);
    expect(parseMoneyInput('2 000 ₽')).toBe(200000);
    expect(parseMoneyInput('2000,5')).toBe(200050);
    expect(parseMoneyInput('2000.05')).toBe(200005);
    expect(parseMoneyInput('2000,')).toBe(200000);
    expect(parseMoneyInput('')).toBe(0);
    expect(parseMoneyInput('  ')).toBe(0);
  });

  it('мусор → null', () => {
    for (const bad of ['abc', '-5', '1,234', '1.2.3', '12e3']) expect(parseMoneyInput(bad)).toBeNull();
  });

  it('обратно в поле ввода', () => {
    expect(moneyInputValue(200000)).toBe('2000');
    expect(moneyInputValue(200050)).toBe('2000,50');
    expect(moneyInputValue(0)).toBe('');
    expect(parseMoneyInput(moneyInputValue(123456))).toBe(123456);
  });
});
