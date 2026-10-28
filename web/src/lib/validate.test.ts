import { describe, expect, it } from 'vitest';
import { cleanCode, emailError, nameError, otpError, passwordError, twoFaCodeError } from './validate';

describe('validate', () => {
  it('email', () => {
    expect(emailError('')).not.toBeNull();
    expect(emailError('alex@selfvoy')).not.toBeNull();
    expect(emailError(' alex@selfvoy.local ')).toBeNull();
  });
  it('пароль: от 8 символов, буквы и цифры (как на сервере)', () => {
    expect(passwordError('a1b2c3')).toMatch(/8/);
    expect(passwordError('abcdefgh')).toMatch(/цифры/);
    expect(passwordError('12345678')).toMatch(/буквы/);
    expect(passwordError('пароль2026')).toBeNull();
    expect(passwordError('Secret123')).toBeNull();
  });
  it('пароль: не длиннее 256 байт UTF-8', () => {
    expect(passwordError('a1'.repeat(128))).toBeNull(); // ровно 256 байт
    expect(passwordError('a1'.repeat(128) + 'b')).toMatch(/256/);
    // Кириллица — 2 байта на букву: 127 букв + 2 цифры = 256 байт, 128 + 1 — уже 257.
    expect(passwordError('ж'.repeat(127) + '12')).toBeNull();
    expect(passwordError('ж'.repeat(128) + '1')).toMatch(/256/);
    // Длина считается в символах, а не в UTF-16: эмодзи — один символ.
    expect(passwordError('a1😀😀😀')).toMatch(/8/);
  });
  it('имя', () => {
    expect(nameError('  ')).not.toBeNull();
    expect(nameError('я'.repeat(81))).not.toBeNull();
    expect(nameError('Алекс')).toBeNull();
  });
  it('код из письма: только цифры, не больше 6', () => {
    expect(cleanCode('123 456')).toBe('123456');
    expect(cleanCode('12-34-56-78')).toBe('123456');
    expect(otpError('12345')).not.toBeNull();
    expect(otpError('123456')).toBeNull();
  });
  it('2FA: TOTP или резервный код', () => {
    expect(twoFaCodeError('123456')).toBeNull();
    expect(twoFaCodeError('abcde-fgh23')).toBeNull();
    expect(twoFaCodeError('12345')).not.toBeNull();
    expect(twoFaCodeError('ABCDEFGHJK')).not.toBeNull();
  });
});
