/*
  Проверка полей форм — те же правила, что на сервере (docs/PROTOCOL.md):
  пароль от 8 символов (до 256 байт UTF-8), буквы (любого алфавита) и цифры; код подтверждения — 6 цифр;
  резервный код 2FA — XXXXX-XXXXX. Возвращают текст ошибки или null.
*/

export function emailError(email: string): string | null {
  const value = email.trim();
  if (!value) return 'Введите email';
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(value)) return 'Похоже, в email опечатка';
  return null;
}

/** Сервер принимает пароль до 256 байт в UTF-8 (кириллица — 2 байта на букву). */
export const PASSWORD_MAX_BYTES = 256;

export function passwordError(password: string): string | null {
  if ([...password].length < 8) return 'Пароль — не короче 8 символов';
  if (!/\p{L}/u.test(password) || !/\d/.test(password)) return 'В пароле нужны и буквы, и цифры';
  if (new TextEncoder().encode(password).length > PASSWORD_MAX_BYTES) return 'Пароль слишком длинный — до 256 байт (около 128 букв кириллицы)';
  return null;
}

export function nameError(name: string): string | null {
  const value = name.trim();
  if (!value) return 'Как вас зовут?';
  if ([...value].length > 80) return 'Имя — до 80 символов';
  return null;
}

/** Оставляет в коде только цифры (вставка «123 456» из письма или приложения). */
export function cleanCode(input: string): string {
  return input.replace(/\D/g, '').slice(0, 6);
}

export function otpError(code: string): string | null {
  return /^\d{6}$/.test(code) ? null : 'Код — 6 цифр';
}

/** Код из приложения-аутентификатора (6 цифр) или резервный код XXXXX-XXXXX. */
export function twoFaCodeError(code: string): string | null {
  const value = code.trim().toUpperCase();
  if (/^\d{6}$/.test(value) || /^[A-Z0-9]{5}-[A-Z0-9]{5}$/.test(value)) return null;
  return 'Введите 6 цифр из приложения или резервный код вида XXXXX-XXXXX';
}
