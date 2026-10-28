/*
  Поле суммы: владелец вводит рубли («2 000,50»), наружу уходят минимальные единицы
  (копейки). Неразборчивый ввод — NaN (форма покажет ошибку).
*/
import { useEffect, useState } from 'react';
import { currencySymbol, moneyInputValue, parseMoneyInput } from '../lib/money';

export function MoneyInput({ value, currency, onChange, label, invalid, width = 150 }: { value: number; currency: string; onChange: (minor: number) => void; label: string; invalid?: boolean; width?: number | string }) {
  const [text, setText] = useState(moneyInputValue(value));
  useEffect(() => {
    // Внешнее изменение (сброс формы) — показать новое значение; свой ввод не трогаем.
    if (!Number.isNaN(value) && parseMoneyInput(text) !== value) setText(moneyInputValue(value));
  }, [value]); // eslint-disable-line react-hooks/exhaustive-deps
  return (
    <span className="input-affix" style={{ width }}>
      <input
        className={`input num ${invalid ? 'invalid' : ''}`}
        inputMode="decimal"
        aria-label={label}
        placeholder="0"
        style={{ height: 38, textAlign: 'right' }}
        value={text}
        onChange={(e) => {
          setText(e.target.value);
          const minor = parseMoneyInput(e.target.value);
          onChange(minor === null ? Number.NaN : minor);
        }}
      />
      <span className="affix">{currencySymbol(currency)}</span>
    </span>
  );
}
