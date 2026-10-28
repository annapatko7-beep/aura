# Установка Selfvoy на сервер (можно только с телефона)

Всё, что нужно сделать руками, — арендовать сервер, вставить одну команду и
(когда будет) вставить API-ключ LLM. Секреты, база, сборка и запуск — скрипт
`deploy/install.sh`, управление — команда `selfvoy`.

## 1. Сервер

Любой VPS с **Ubuntu 22.04 или 24.04**, 2–4 ГБ памяти, 20+ ГБ диска (Hetzner,
DigitalOcean, Timeweb Cloud, Selectel, Yandex Cloud…). На 2 ГБ скрипт сам
добавит swap — сборка C++-сервера требует памяти.

С телефона: приложение **Termius** → New Host → IP сервера, пользователь
`root`, пароль из письма провайдера → Connect.

## 2. Установка — одна команда

```bash
curl -fsSL https://raw.githubusercontent.com/velorievvork-pixel/Aura/main/deploy/install.sh | sudo bash
```

Пока изменения не влиты в `main`, берите ветку:

```bash
curl -fsSL https://raw.githubusercontent.com/velorievvork-pixel/Aura/claude/booking-referrals/deploy/install.sh \
  | sudo SELFVOY_BRANCH=claude/booking-referrals bash
```

Приватный репозиторий: `raw.githubusercontent.com` и `git clone` потребуют
токен GitHub (Settings → Developer settings → Fine-grained token, доступ
Contents: Read к репозиторию):

```bash
T=ваш_токен
curl -fsSL -H "Authorization: token $T" \
  https://raw.githubusercontent.com/velorievvork-pixel/Aura/main/deploy/install.sh \
  | sudo SELFVOY_REPO=https://$T@github.com/velorievvork-pixel/Aura.git bash
```

Скрипт спросит **API-ключ LLM** — вставьте или нажмите Enter (тогда работает
встроенная заглушка, ключ можно вставить позже). Первая сборка — 5–15 минут.
В конце он напечатает **адрес сервера** для приложения: `ws://IP:9000`.

### С доменом и HTTPS (для настоящих пользователей)

A-запись домена (например, `api.selfvoy.example.com`) → IP сервера, затем:

```bash
curl -fsSL …/deploy/install.sh | sudo SELFVOY_DOMAIN=api.selfvoy.example.com bash
```

Caddy сам получит сертификат Let's Encrypt; адрес приложения —
`wss://api.selfvoy.example.com`, порт 9000 наружу не открыт, адрес клиента для лимитов
берётся из `X-Forwarded-For` только от Caddy (`deploy/compose.tls.yml`).

## 3. Телефон

Selfvoy → экран входа → «Адрес сервера» → вставить адрес из шага 2 →
«Сохранить адрес» → «Создать аккаунт».

Почта по умолчанию не отправляется (`SELFVOY_MAIL_DRIVER=log`) — подтвердите
email на сервере:

```bash
selfvoy verify ваш@email
```

Настоящая почта: в `/opt/selfvoy/.env` — `SELFVOY_MAIL_DRIVER=smtp` и `SELFVOY_SMTP_*`
(см. `.env.example`), затем `selfvoy restart server`.

## 4. API-ключ LLM — вставить или сменить

```bash
selfvoy llm-key
```

Спросит ключ (ввод скрыт), адрес API (Enter — OpenAI; для другого
OpenAI-совместимого провайдера — его адрес, например `https://…/v1`) и модель
(Enter — текущая). Перезапустит AI-сервис и сразу проверит модель.

Проверка в любой момент:

```bash
selfvoy llm-check
```

Пять типичных запросов (напоминание, расписание, встреча, память, разговор):
для каждого — кто ответил, время, намерение, ответ. «…-fallback» — модель не
ответила и сработали локальные правила (неверный ключ, нет доступа к API,
баланс, адрес или имя модели).

## 5. Команды

| Команда | Что делает |
|---|---|
| `selfvoy status` | контейнеры и текущая модель LLM |
| `selfvoy logs [server\|ai\|postgres\|caddy]` | последние логи |
| `selfvoy verify EMAIL` | подтвердить email без почты |
| `selfvoy pro EMAIL DAYS` | выдать тариф Pro на DAYS дней (0–3650; 0 — снять Pro) |
| `selfvoy llm-key` / `selfvoy llm-check` | вставить ключ / проверить модель |
| `selfvoy backup` | копия базы в `/opt/selfvoy/backups/` (14 последних) |
| `selfvoy update` | обновить и пересобрать |
| `selfvoy restart [сервис]` | перезапуск |

Секреты — только в `/opt/selfvoy/.env` (права 600, в git не попадает).

## Сервер, установленный до переименования (Aura)

Продукт раньше назывался Aura: каталог `/opt/aura`, команда `aura`, переменные
`AURA_*`, база `aura` и роли `aura_app`/`aura_ai`. Чтобы перейти на новые
имена, повторите установочную команду из шага 2 — скрипт:

- останется в `/opt/aura` (там `.env`, а имя каталога — имя проекта compose,
  по которому находится том PostgreSQL);
- переименует ключи `AURA_*` в `.env` в `SELFVOY_*`;
- переименует базу `aura` → `selfvoy` и роли → `selfvoy_app`/`selfvoy_ai`
  (данные и пароли сохраняются);
- поставит команду `selfvoy` вместо `aura`.

То же вручную на обновлённом проекте: `bash deploy/selfvoy.sh migrate`.
Сервер и AI-сервис один релиз ещё читают старые `AURA_*`, если `SELFVOY_*`
не заданы (с предупреждением в логе), но `docker-compose.yml` передаёт
контейнерам только `SELFVOY_*`.

## Облачная сессия Claude: проверка с настоящей LLM

Чтобы ассистент проверил модель прямо в облачной сессии, в настройках
облачного окружения (меню окружения в заголовке сессии → Edit):

- **Network secrets / переменные окружения:** `OPENAI_API_KEY`,
  `SELFVOY_LLM_PROVIDER=openai`, при необходимости `SELFVOY_LLM_MODEL` и
  `OPENAI_BASE_URL` (для не-OpenAI).
- **Network access → Allowed domains:** домен API (`api.openai.com` или
  провайдера); `*.blob.core.windows.net` — чтобы ассистент мог присылать APK
  из GitHub Actions файлом.

Новая сессия подхватит их; дальше: `make llm-check`.
