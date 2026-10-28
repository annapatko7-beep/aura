// Сквозной сценарий страниц записи и рефералов в веб-клиенте против настоящего сервера
// (Playwright, Chromium): владелец заводит страницу → гость записывается на серию →
// переносит и отменяет по ссылке управления → владелец видит записи → регистрация
// друга по реферальной ссылке засчитывается владельцу.
//
//   npx vite --port 5173 &               # клиент; сервер с SELFVOY_MAIL_DRIVER=dev
//   node scripts/booking-ui-e2e.mjs [папка для снимков]
//
// Переменные: SELFVOY_UI_URL (http://localhost:5173), CHROMIUM (путь к браузеру).
// Нужен работающий AI-сервис: свободное время подбирает он.
import { existsSync, mkdirSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { chromium } from 'playwright-core';

const BASE = process.env.SELFVOY_UI_URL ?? 'http://localhost:5173';
const OUT = process.argv[2] ?? 'ui-shots';
mkdirSync(OUT, { recursive: true });

function chromiumPath() {
  if (process.env.CHROMIUM) return process.env.CHROMIUM;
  const root = process.env.PLAYWRIGHT_BROWSERS_PATH ?? '/opt/pw-browsers';
  const dir = readdirSync(root).find((d) => /^chromium-\d+$/.test(d));
  const bin = dir && join(root, dir, 'chrome-linux', 'chrome');
  return bin && existsSync(bin) ? bin : undefined;
}

let failures = 0;
const check = (ok, label, detail = '') => {
  console.log(`  ${ok ? '✓' : '✗'} ${label}${!ok && detail ? ` — ${detail}` : ''}`);
  if (!ok) failures += 1;
};

const browser = await chromium.launch({ executablePath: chromiumPath() });
const run = Date.now().toString(36);

async function open(viewport, tag) {
  const context = await browser.newContext({ viewport, deviceScaleFactor: 1, isMobile: viewport.width < 600, hasTouch: viewport.width < 600 });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  let n = 0;
  const shot = (name) => page.screenshot({ path: join(OUT, `${tag}-${String(++n).padStart(2, '0')}-${name}.png`), fullPage: true });
  const press = async (name) => {
    const button = page.getByRole('button', { name, exact: true }).first();
    await button.evaluate((el) => el.scrollIntoView({ block: 'center' }));
    await button.click();
  };
  return { context, page, errors, shot, press };
}

async function signup(page, press, name, email, password, path = '/signup') {
  await page.goto(`${BASE}/#${path}`);
  await page.getByLabel('Имя').waitFor({ timeout: 20000 });
  await page.getByLabel('Имя').fill(name);
  await page.getByLabel('Email').fill(email);
  await page.locator('input[autocomplete="new-password"]').fill(password);
  await press('Продолжить');
  await page.getByRole('heading', { name: 'Подтвердите email' }).waitFor({ timeout: 15000 });
  const code = (await page.locator('.notice b.mono').textContent())?.trim() ?? '';
  await page.getByLabel('Код из письма').fill(code);
  await press('Подтвердить');
  await page.getByText('Ближайшие задачи').waitFor({ timeout: 20000 });
}

console.log('\nВладелец (ноутбук 1280×860): страница записи');
const owner = await open({ width: 1280, height: 860 }, 'owner');
await signup(owner.page, owner.press, 'Ольга Коуч', `coach.${run}@example.com`, `Selfvoy${run}9`);
await owner.page.locator('.sidebar a', { hasText: 'Запись' }).click();
await owner.page.getByRole('heading', { name: 'Запись' }).waitFor();
await owner.press('Новая страница');
await owner.press('Создать страницу');
check(await owner.page.getByText('Назовите страницу').isVisible(), 'пустое название не уходит на сервер');
await owner.page.getByLabel('Название').fill('Консультация');
await owner.page.getByLabel('Описание для гостей').fill('Разберём ваш проект за час.');
await owner.page.getByRole('radio', { name: '1 час' }).click();
await owner.page.getByLabel('Серия, встреч').fill('3');
await owner.shot('form');
await owner.press('Создать страницу');
const linkInput = owner.page.getByLabel('Ссылка на страницу записи');
await linkInput.waitFor({ timeout: 30000 });
const pageLink = await linkInput.inputValue();
check(/#\/b\/[a-z0-9]{10}$/.test(pageLink), 'страница создана, есть ссылка', pageLink);
await owner.shot('pages');

console.log('\nГость (телефон 390×844): запись на серию');
const guest = await open({ width: 390, height: 844 }, 'guest');
await guest.page.goto(pageLink);
await guest.page.getByRole('heading', { name: 'Консультация' }).waitFor({ timeout: 30000 });
check(await guest.page.getByText('Ольга Коуч').first().isVisible(), 'гость видит имя организатора');
const slots = guest.page.locator('.invite-slot');
check((await slots.count()) > 0, 'показано свободное время', String(await slots.count()));
await guest.shot('page');
await guest.press('Записаться');
check(await guest.page.getByText('Выберите время').last().isVisible(), 'без времени запись не уходит');
await slots.first().click();
await guest.page.getByLabel('Сколько встреч').selectOption('2');
await guest.page.getByLabel('Ваше имя').fill('Пётр');
await guest.page.getByLabel('Email').fill(`petr.${run}@example.com`);
await guest.press('Записаться');
await guest.page.getByRole('heading', { name: 'Вы записаны' }).waitFor({ timeout: 30000 });
const manageLink = await guest.page.getByLabel('Ссылка управления записью').inputValue();
check(/#\/m\/[A-Za-z0-9_-]{43}$/.test(manageLink), 'выдана ссылка управления', manageLink);
check(await guest.page.getByRole('button', { name: 'Добавить в календарь' }).isVisible(), 'есть кнопка «Добавить в календарь»');
await guest.shot('booked');

console.log('\nГость: перенос и отмена по ссылке управления');
await guest.page.goto(manageLink);
await guest.page.getByRole('button', { name: 'Перенести', exact: true }).waitFor({ timeout: 30000 });
await guest.shot('manage');
await guest.press('Перенести');
await guest.page.locator('.invite-slot').nth(1).click();
await guest.shot('reschedule');
await guest.press('Перенести сюда');
await guest.page.getByText('Встреча перенесена.').waitFor({ timeout: 30000 });
check(true, 'встреча перенесена');
await guest.press('Отменить');
await guest.page.getByLabel('Что отменить').waitFor();
await guest.page.getByLabel('Причина').fill('Планы изменились');
await guest.shot('cancel');
await guest.press('Да, отменить');
await guest.page.getByRole('heading', { name: 'Запись отменена' }).waitFor({ timeout: 30000 });
check(await guest.page.getByRole('link', { name: 'Записаться снова' }).isVisible(), 'после отмены — «Записаться снова»');
await guest.shot('cancelled');

console.log('\nВладелец: записи гостей и рефералы');
await owner.page.reload();
await owner.page.getByText('Пётр').first().waitFor({ timeout: 20000 });
check(await owner.page.getByText('Отменил гость').isVisible(), 'владелец видит запись, отменённую гостем');
check(await owner.page.getByText(`petr.${run}@example.com`).isVisible(), 'владелец видит email гостя');
await owner.shot('bookings');
await owner.page.locator('.sidebar a', { hasText: 'Настройки' }).first().click();
await owner.press('Пригласить друзей');
const refInput = owner.page.getByLabel('Реферальная ссылка');
await refInput.waitFor({ timeout: 20000 });
const refLink = await refInput.inputValue();
check(/#\/signup\?ref=[A-Z0-9]{8}$/.test(refLink), 'реферальная ссылка', refLink);

const friend = await open({ width: 390, height: 844 }, 'friend');
await friend.page.goto(refLink);
await friend.page.getByText('Вас пригласили в Selfvoy').waitFor({ timeout: 20000 });
check(true, 'регистрация по ссылке показывает приглашение');
await friend.shot('signup');
await signup(friend.page, friend.press, 'Фёдор', `fedor.${run}@example.com`, `Selfvoy${run}8`, refLink.slice(refLink.indexOf('#') + 1));
await owner.page.reload();
await owner.press('Пригласить друзей');
await owner.page.getByText('Фёдор').waitFor({ timeout: 20000 });
check(await owner.page.getByText('С Selfvoy').isVisible(), 'друг засчитан после подтверждения email');
await owner.shot('referrals');

for (const [who, ctx] of [['владелец', owner], ['гость', guest], ['друг', friend]]) {
  check(ctx.errors.length === 0, `${who}: без ошибок JS`, ctx.errors.join('; '));
}
await browser.close();
console.log(failures ? `\nошибок: ${failures}` : '\nUI booking: OK');
process.exit(failures ? 1 : 0);
