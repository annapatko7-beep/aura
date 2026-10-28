// Сквозной сценарий веб-клиента против настоящего сервера Selfvoy (Playwright, Chromium):
// регистрация → код из письма → 2FA (TOTP по секрету) → выход → вход с 2FA →
// приватность → переговоры → диалог с Selfvoy → экран ожидания при недоступном сервере.
//
//   npx vite --port 5173 &               # клиент (или сборка с CSP: npx vite build && npx vite preview)
//   node scripts/ui-e2e.mjs [папка для снимков]
//
// Переменные: SELFVOY_UI_URL (http://localhost:5173), SELFVOY_UI_PEER (email собеседника
// для переговоров, по умолчанию dima@selfvoy.local из tools/demo_seed.py),
// CHROMIUM (путь к браузеру; по умолчанию — из PLAYWRIGHT_BROWSERS_PATH).
// Сервер должен работать с SELFVOY_MAIL_DRIVER=dev: код из письма берётся с экрана.
import { createHmac } from 'node:crypto';
import { existsSync, mkdirSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { chromium } from 'playwright-core';

const BASE = process.env.SELFVOY_UI_URL ?? 'http://localhost:5173';
const PEER = process.env.SELFVOY_UI_PEER ?? 'dima@selfvoy.local';
const OUT = process.argv[2] ?? 'ui-shots';
mkdirSync(OUT, { recursive: true });

function chromiumPath() {
  if (process.env.CHROMIUM) return process.env.CHROMIUM;
  const root = process.env.PLAYWRIGHT_BROWSERS_PATH ?? '/opt/pw-browsers';
  const dir = readdirSync(root).find((d) => /^chromium-\d+$/.test(d));
  const bin = dir && join(root, dir, 'chrome-linux', 'chrome');
  return bin && existsSync(bin) ? bin : undefined;
}

// RFC 6238: base32-секрет → 6-значный код для момента t (сек).
function totp(secret, t = Date.now() / 1000) {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
  let bits = '';
  for (const ch of secret.replace(/[\s=]/g, '').toUpperCase()) bits += alphabet.indexOf(ch).toString(2).padStart(5, '0');
  const key = Buffer.from(bits.match(/.{8}/g).map((b) => parseInt(b, 2)));
  const counter = Buffer.alloc(8);
  counter.writeBigUInt64BE(BigInt(Math.floor(t / 30)));
  const hmac = createHmac('sha1', key).update(counter).digest();
  const offset = hmac[hmac.length - 1] & 0xf;
  const code = (hmac.readUInt32BE(offset) & 0x7fffffff) % 1_000_000;
  return String(code).padStart(6, '0');
}

let failures = 0;
const check = (ok, label, detail = '') => {
  console.log(`  ${ok ? '✓' : '✗'} ${label}${!ok && detail ? ` — ${detail}` : ''}`);
  if (!ok) failures += 1;
};

const browser = await chromium.launch({ executablePath: chromiumPath() });
const run = Date.now().toString(36);
const email = `ui.${run}@example.com`;
const password = `Selfvoy${run}9`;

async function scenario(viewport, tag) {
  const context = await browser.newContext({ viewport, deviceScaleFactor: 2, isMobile: viewport.width < 600, hasTouch: viewport.width < 600 });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  // Сборка (vite preview) идёт с Content-Security-Policy: нарушения — тоже ошибки.
  page.on('console', (m) => m.type() === 'error' && /Content Security Policy/i.test(m.text()) && errors.push(m.text()));
  let n = 0;
  const shot = (name) => page.screenshot({ path: join(OUT, `${tag}-${String(++n).padStart(2, '0')}-${name}.png`) });
  // Кнопку — в центр экрана: у нижнего края её закрывает плавающая панель вкладок.
  page.press = async (name) => {
    const button = page.getByRole('button', { name, exact: true }).first();
    await button.evaluate((el) => el.scrollIntoView({ block: 'center' }));
    await button.click();
  };
  const nav = (label) => page.locator(viewport.width < 900 ? '.tab-bar a' : '.sidebar a', { hasText: label }).first().click();
  return { context, page, errors, shot, nav };
}

console.log('\nТелефон 360×760: регистрация, 2FA, вход');
{
  const { context, page, errors, shot, nav } = await scenario({ width: 360, height: 760 }, 'phone');
  await page.goto(`${BASE}/#/login`);
  await page.getByRole('heading', { name: 'Вход в Selfvoy' }).waitFor({ timeout: 20000 });
  await shot('login');

  // Валидация на клиенте: слабый пароль не уходит на сервер.
  await page.getByRole('link', { name: 'Зарегистрироваться' }).click();
  await page.getByLabel('Имя').fill('Тест Интерфейса');
  await page.getByLabel('Email').fill(email);
  await page.locator('input[autocomplete="new-password"]').fill('short');
  await page.press('Продолжить');
  check(await page.getByText('Пароль — не короче 8 символов').isVisible(), 'регистрация: короткий пароль отклонён до отправки');
  await shot('signup-validation');
  await page.locator('input[autocomplete="new-password"]').fill(password);
  await page.press('Продолжить');

  await page.getByRole('heading', { name: 'Подтвердите email' }).waitFor({ timeout: 15000 });
  const devCode = (await page.locator('.notice b.mono').textContent())?.trim() ?? '';
  check(/^\d{6}$/.test(devCode), 'код подтверждения показан (сервер в dev-режиме почты)', devCode);
  await shot('verify');
  await page.getByLabel('Код из письма').fill(devCode);
  await page.press('Подтвердить');
  await page.getByText('Ближайшие задачи').waitFor({ timeout: 20000 });
  check(true, 'после подтверждения email — вход и главная');
  await shot('home');

  // Включение 2FA: секрет → код → резервные коды.
  await page.locator('a[aria-label="Настройки"]').click();
  await page.press('Безопасность');
  await page.press('Включить 2FA');
  await page.locator('img.qr').waitFor();
  const secret = ((await page.locator('code.secret').textContent()) ?? '').replace(/\s/g, '');
  check(secret.length >= 16, 'показаны QR-код и ключ для приложения', secret);
  await shot('2fa-setup');
  await page.getByLabel('Код из приложения').fill(totp(secret));
  await page.press('Подтвердить и включить');
  await page.locator('.codes').waitFor({ timeout: 15000 });
  const codes = await page.locator('.codes span').count();
  check(codes === 10, '2FA включена, показаны 10 резервных кодов', String(codes));
  await shot('2fa-recovery');
  await page.press('Я сохранил коды');
  check(await page.getByText('Включена').isVisible(), 'статус 2FA — «Включена»');

  // Выход и вход с кодом из приложения (следующее окно — код одноразовый).
  await page.press('Приложение');
  await page.press('Выйти из аккаунта');
  await page.getByRole('heading', { name: 'Вход в Selfvoy' }).waitFor({ timeout: 10000 });
  await page.getByLabel('Email').fill(email);
  await page.locator('input[autocomplete="current-password"]').fill('wrong-pass-1');
  await page.press('Войти');
  await page.locator('.notice.error').waitFor({ timeout: 10000 });
  check((await page.locator('.notice.error').textContent())?.includes('Неверный'), 'неверный пароль — понятная ошибка');
  await page.locator('input[autocomplete="current-password"]').fill(password);
  await page.press('Войти');
  await page.getByRole('heading', { name: 'Двухфакторная защита' }).waitFor({ timeout: 10000 });
  await shot('2fa-login');
  await page.getByLabel('Код из приложения').fill('000000');
  await page.press('Войти');
  await page.locator('.field-error').waitFor({ timeout: 10000 });
  check(true, 'неверный код 2FA отклонён');
  await page.getByLabel('Код из приложения').fill(totp(secret, Date.now() / 1000 + 30));
  await page.press('Войти');
  await page.getByText('Ближайшие задачи').waitFor({ timeout: 15000 });
  check(true, 'вход с кодом 2FA');

  // Приватность ИИ: переключатель сохраняется на сервере.
  await page.locator('a[aria-label="Настройки"]').click();
  await page.press('Приватность ИИ');
  const placeSwitch = page.getByRole('switch', { name: 'Разрешить выбор локаций' });
  await placeSwitch.click();
  await page.waitForTimeout(800);
  await shot('privacy');
  await page.reload();
  await page.press('Приватность ИИ');
  check((await page.getByRole('switch', { name: 'Разрешить выбор локаций' }).getAttribute('aria-checked')) === 'false', 'приватность: «выбор локаций» выключен и пережил перезагрузку');
  await page.getByRole('switch', { name: 'Разрешить выбор локаций' }).click();
  await page.waitForTimeout(500);

  // Переговоры с демо-пользователем.
  await nav('Переговоры');
  await page.getByPlaceholder('email собеседника').fill(PEER);
  await page.getByPlaceholder('Например, обсудить стартап').fill('обсудить стартап');
  await shot('negotiation-form');
  await page.press('Начать переговоры');
  await page.getByText('Как договорились').waitFor({ timeout: 60000 });
  check(true, 'переговоры: таймлайн шагов');
  await shot('negotiation-result');

  // Диалог с Selfvoy.
  await nav('Selfvoy');
  await page.getByLabel('Запрос к Selfvoy').fill('Что у меня сегодня?');
  await page.press('Отправить');
  // Приветствие исчезает с первым сообщением: ждём своё сообщение и ответ после него.
  await page.locator('.msg.mine').first().waitFor({ timeout: 10000 });
  await page.locator('.msg.mine ~ .msg.theirs').first().waitFor({ timeout: 60000 });
  check(true, 'диалог: ответ Selfvoy получен');
  await shot('chat');

  await nav('Задачи');
  await page.getByPlaceholder('Новая задача').fill('Позвонить юристу');
  await page.press('Добавить');
  await page.getByText('Позвонить юристу').waitFor({ timeout: 10000 });
  check(true, 'задачи: добавление');
  await shot('tasks');

  check(errors.length === 0, 'без ошибок JavaScript на странице', errors.join(' | '));
  await context.close();
}

console.log('\nДесктоп 1280×800: вход, разделы');
{
  const { context, page, errors, shot, nav } = await scenario({ width: 1280, height: 800 }, 'desktop');
  await page.goto(`${BASE}/#/login`);
  await page.getByRole('heading', { name: 'Вход в Selfvoy' }).waitFor({ timeout: 20000 });
  await shot('login');
  await page.getByLabel('Email').fill(email);
  await page.locator('input[autocomplete="current-password"]').fill(password);
  await page.press('Войти');
  await page.getByRole('heading', { name: 'Двухфакторная защита' }).waitFor({ timeout: 10000 });
  check(true, 'новое устройство снова спрашивает код 2FA');
  await context.close();
  check(errors.length === 0, 'без ошибок JavaScript (десктоп)', errors.join(' | '));
  void nav;
}

console.log('\nСервер недоступен: экран ожидания');
{
  const { context, page, shot } = await scenario({ width: 360, height: 760 }, 'offline');
  await page.addInitScript(() => localStorage.setItem('selfvoy.server', 'ws://127.0.0.1:1'));
  await page.goto(`${BASE}/#/login`);
  await page.getByRole('button', { name: 'Повторить' }).waitFor({ timeout: 20000 });
  check(await page.getByText(/сервер/i).first().isVisible(), 'вместо ошибки — экран «Поиск соединения» с «Повторить»');
  await shot('connection');
  await context.close();
}

await browser.close();
console.log(`\nснимки: ${OUT}; ошибок: ${failures}`);
process.exit(failures ? 1 : 0);
