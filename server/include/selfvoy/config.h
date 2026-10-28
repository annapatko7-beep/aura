// selfvoy/config.h — конфигурация сервера из переменных окружения.
#pragma once

#include <string>
#include <vector>

namespace selfvoy {

struct Config {
    std::string host = "0.0.0.0";              // SELFVOY_HOST
    int port = 9000;                           // SELFVOY_PORT
    std::string databaseUrl;                   // SELFVOY_DATABASE_URL (postgres://...)
    std::string embeddedDbPath = ".selfvoy-data.json";  // SELFVOY_EMBEDDED_DB (режим без PostgreSQL)

    std::string aiServiceUrl = "http://127.0.0.1:8000";  // SELFVOY_AI_URL
    std::string aiServiceToken;                          // SELFVOY_AI_TOKEN
    int aiTimeoutMs = 15000;                             // SELFVOY_AI_TIMEOUT_MS

    // Режим разработки (SELFVOY_DEV_MODE=1): разрешает драйвер почты dev (коды в
    // ответах API) и слабый/пустой SELFVOY_JWT_SECRET (тогда генерируется случайный
    // секрет на время жизни процесса). Вне dev-режима сервер с такими настройками
    // не стартует — см. Config::validate().
    bool devMode = false;                                // SELFVOY_DEV_MODE

    std::string jwtSecret;                               // SELFVOY_JWT_SECRET (>= 32 байт; обязателен вне dev)
    std::string twoFactorKey;                            // SELFVOY_2FA_KEY (ключ AES-256-GCM для TOTP-секретов; обязателен вне dev)
    long long tokenTtlSeconds = 60 * 60 * 24 * 7;        // SELFVOY_TOKEN_TTL
    long long refreshTtlSeconds = 60 * 60 * 24 * 30;     // SELFVOY_REFRESH_TTL
    long long verifyTtlSeconds = 300;                    // SELFVOY_VERIFY_TTL (код подтверждения email: 5 минут)
    long long resetTtlSeconds = 300;                     // SELFVOY_RESET_TTL (код сброса пароля: 5 минут)
    // dev — код в ответе API (только при SELFVOY_DEV_MODE=1); log — в лог пишется
    // лишь факт выдачи кода (без самого кода); smtp — настоящие письма.
    std::string mailDriver = "log";                      // SELFVOY_MAIL_DRIVER: dev|log|smtp
    // SMTP (SELFVOY_MAIL_DRIVER=smtp): коды подтверждения и сброса уходят письмом.
    std::string smtpHost;                                // SELFVOY_SMTP_HOST
    int smtpPort = 587;                                  // SELFVOY_SMTP_PORT
    std::string smtpUser;                                // SELFVOY_SMTP_USER
    std::string smtpPassword;                            // SELFVOY_SMTP_PASSWORD
    std::string smtpFrom;                                // SELFVOY_SMTP_FROM («Selfvoy <no-reply@домен>»)
    std::string smtpTls = "starttls";                    // SELFVOY_SMTP_TLS: starttls|tls|none
    std::string smtpCaFile;                              // SELFVOY_SMTP_CA_FILE (свой корневой сертификат)
    int smtpTimeoutMs = 15000;                           // SELFVOY_SMTP_TIMEOUT_MS
    std::string toolsMode = "sandbox";                   // SELFVOY_TOOLS_MODE: sandbox|http
    std::string emailApiUrl;                             // SELFVOY_EMAIL_API_URL
    std::string mapsApiUrl;                              // SELFVOY_MAPS_API_URL
    std::string bookingApiUrl;                           // SELFVOY_BOOKING_API_URL


    std::size_t maxFrameBytes = 1024 * 1024;             // SELFVOY_MAX_FRAME
    std::size_t maxPreAuthFrameBytes = 64 * 1024;        // SELFVOY_MAX_PREAUTH_FRAME (кадр до входа)
    std::size_t maxConnections = 1024;                   // SELFVOY_MAX_CONNECTIONS
    std::size_t maxConnectionsPerIp = 20;                // SELFVOY_MAX_CONNECTIONS_PER_IP (0 = без лимита)
    int maxLoginAttempts = 8;                            // SELFVOY_MAX_LOGIN_ATTEMPTS (на email)
    int maxIpAttempts = 30;                              // SELFVOY_MAX_IP_ATTEMPTS (неудачи с одного IP, 0 = без лимита)
    int loginWindowSec = 300;                            // SELFVOY_LOGIN_WINDOW
    int maxTwoFactorAttempts = 5;                        // SELFVOY_MAX_2FA_ATTEMPTS (неверные TOTP на аккаунт)
    int twoFactorWindowSec = 900;                        // SELFVOY_2FA_WINDOW
    int maxCodeAttempts = 5;                             // SELFVOY_MAX_CODE_ATTEMPTS (неверные коды email/сброса → код сгорает)
    int mailPerEmailPerHour = 3;                         // SELFVOY_MAIL_PER_EMAIL_HOUR (resendCode/forgotPassword на адрес)
    int mailPerIpPerHour = 20;                           // SELFVOY_MAIL_PER_IP_HOUR
    int registerPerIpPerHour = 10;                       // SELFVOY_REGISTER_PER_IP_HOUR (0 = без лимита)
    int pingIntervalSec = 25;                            // SELFVOY_PING_INTERVAL
    int socketTimeoutSec = 120;                          // SELFVOY_SOCKET_TIMEOUT
    int authTimeoutSec = 30;                             // SELFVOY_AUTH_TIMEOUT (неавторизованное соединение закрывается)
    int authRecheckSec = 15;                             // SELFVOY_AUTH_RECHECK (перепроверка сессии открытого соединения)
    int schedulerIntervalMs = 30000;                     // SELFVOY_SCHEDULER_INTERVAL_MS (0 = выключен)

    // Интеграции (этап 9): Google OAuth 2.0 + PKCE. URL по умолчанию —
    // реальные эндпоинты Google (прод идёт за TLS-прокси); e2e подменяет
    // их локальным mock-сервером.
    std::string googleClientId;                          // SELFVOY_GOOGLE_CLIENT_ID
    std::string googleClientSecret;                      // SELFVOY_GOOGLE_CLIENT_SECRET
    std::string googleAuthUrl = "https://accounts.google.com/o/oauth2/v2/auth";  // SELFVOY_GOOGLE_AUTH_URL
    std::string googleTokenUrl = "https://oauth2.googleapis.com/token";          // SELFVOY_GOOGLE_TOKEN_URL
    std::string googleRevokeUrl = "https://oauth2.googleapis.com/revoke";        // SELFVOY_GOOGLE_REVOKE_URL
    std::string googleCalendarUrl = "https://www.googleapis.com/calendar/v3";    // SELFVOY_GOOGLE_CALENDAR_URL
    std::string googleGmailUrl = "https://gmail.googleapis.com/gmail/v1";        // SELFVOY_GOOGLE_GMAIL_URL
    std::string googleUserinfoUrl = "https://openidconnect.googleapis.com/v1/userinfo"; // SELFVOY_GOOGLE_USERINFO_URL
    int oauthStateTtlSec = 600;                          // SELFVOY_OAUTH_STATE_TTL — жизнь одноразового state

    // Вход/регистрация через OAuth: Google, GitHub, Yandex, VK ID, Discord
    std::string googleAuthClientId;                      // SELFVOY_OAUTH_GOOGLE_CLIENT_ID (fallback: googleClientId)
    std::string googleAuthClientSecret;                  // SELFVOY_OAUTH_GOOGLE_CLIENT_SECRET (fallback: googleClientSecret)

    std::string githubClientId;                          // SELFVOY_GITHUB_CLIENT_ID
    std::string githubClientSecret;                      // SELFVOY_GITHUB_CLIENT_SECRET
    std::string githubAuthUrl = "https://github.com/login/oauth/authorize";
    std::string githubTokenUrl = "https://github.com/login/oauth/access_token";
    std::string githubUserUrl = "https://api.github.com/user";

    std::string yandexClientId;                          // SELFVOY_YANDEX_CLIENT_ID
    std::string yandexClientSecret;                      // SELFVOY_YANDEX_CLIENT_SECRET
    std::string yandexAuthUrl = "https://oauth.yandex.ru/authorize";
    std::string yandexTokenUrl = "https://oauth.yandex.ru/token";
    std::string yandexUserUrl = "https://login.yandex.ru/info";

    std::string vkClientId;                              // SELFVOY_VK_CLIENT_ID
    std::string vkClientSecret;                          // SELFVOY_VK_CLIENT_SECRET
    std::string vkAuthUrl = "https://id.vk.com/authorize";
    std::string vkTokenUrl = "https://id.vk.com/oauth2/auth";
    std::string vkUserUrl = "https://id.vk.com/oauth2/user_info";

    std::string discordClientId;                         // SELFVOY_DISCORD_CLIENT_ID
    std::string discordClientSecret;                     // SELFVOY_DISCORD_CLIENT_SECRET
    std::string discordAuthUrl = "https://discord.com/api/oauth2/authorize";
    std::string discordTokenUrl = "https://discord.com/api/oauth2/token";
    std::string discordUserUrl = "https://discord.com/api/users/@me";

    bool oauthDevMock = false;                           // SELFVOY_E2E_OAUTH_MOCK / SELFVOY_OAUTH_DEV_MOCK

    // Push-уведомления (этап 13). Драйверы:
    //   dev     — писать payload в лог (разработка/e2e без шлюза);
    //   webhook — POST на SELFVOY_PUSH_WEBHOOK_URL (push-шлюз доставляет в APNs;
    //             сам шлюз терминирует TLS и хранит ключ APNs).
    // Реальный APNs требует HTTP/2+TLS и JWT ES256, которых нет в net.h/crypto
    // намеренно — поэтому доставка только через внешний шлюз (честно, без имитации).
    std::string pushDriver = "dev";                      // SELFVOY_PUSH_DRIVER: dev|webhook
    std::string pushWebhookUrl;                          // SELFVOY_PUSH_WEBHOOK_URL
    std::string apnsTopic = "app.selfvoy";               // SELFVOY_APNS_TOPIC (bundle id iOS)
    std::string apnsUrl = "https://api.push.apple.com";  // SELFVOY_APNS_URL (для шлюза)
    // FCM (Android, этап 11): та же схема — сервер не хранит ключи Google и не
    // терминирует TLS; шлюз подписывает OAuth2-токен сервисного аккаунта и
    // форвардит тело fcm.message в FCM HTTP v1 API.
    std::string fcmUrl = "https://fcm.googleapis.com/v1/projects/selfvoy/messages:send";  // SELFVOY_FCM_URL
    std::string logLevel = "info";                       // SELFVOY_LOG_LEVEL
    // Админ-панель: email будущих администраторов через запятую/пробел.
    // Роль admin выдаётся только подтверждённому адресу (при старте и при входе).
    std::string adminEmails;                             // SELFVOY_ADMIN_EMAILS
    // IP обратных прокси (TLS-терминатор), которым верим в X-Forwarded-For, через запятую:
    // за ними лимиты гостей и аудит считают по адресу клиента, а не прокси.
    std::string trustedProxies;                          // SELFVOY_TRUSTED_PROXIES
    // Адрес веб-клиента для ссылок-приглашений (invite.create → url):
    // https://selfvoy.example.com → https://selfvoy.example.com/#/i/<токен>. Пусто — ссылку
    // строит клиент.
    std::string publicUrl;                               // SELFVOY_PUBLIC_URL
    // Внешний адрес самого сервера (HTTP-маршруты на порту WebSocket), например
    // https://api.example.com: calendar.feed* отдают url = <адрес>/ics/<токен>.ics.
    // Пусто — url пуст, клиент собирает адрес из адреса сервера (wss→https, ws→http).
    std::string serverPublicUrl;                         // SELFVOY_SERVER_PUBLIC_URL

    // --- Рост (S1a): тариф, https, Telegram-бот ---
    // 0 (по умолчанию, бета) — функции Pro открыты всем; 1 — требуют действующего Pro
    // (users.plan_until > now), иначе ошибка pro_required.
    bool plansEnforced = false;                          // SELFVOY_PLANS_ENFORCED
    // Дополнительный корневой сертификат для исходящих https-запросов (свой CA, тесты).
    std::string caFile;                                  // SELFVOY_CA_FILE
    // Telegram-бот (F2): включён, когда заданы токен и имя (без @).
    std::string telegramBotToken;                        // SELFVOY_TELEGRAM_BOT_TOKEN
    std::string telegramBotUsername;                     // SELFVOY_TELEGRAM_BOT_USERNAME
    std::string telegramApiUrl = "https://api.telegram.org";  // SELFVOY_TELEGRAM_API_URL (e2e — http-мок)
    bool telegramPoll = true;                            // SELFVOY_TELEGRAM_POLL (0 — только отправка)
    // --- Рост (S2a): оплата записи (F4) ---
    // API ЮKassa (владелец подключает свой магазин; в e2e — http-мок).
    std::string yookassaApiUrl = "https://api.yookassa.ru/v3";  // SELFVOY_YOOKASSA_API_URL

    bool telegramEnabled() const { return !telegramBotToken.empty() && !telegramBotUsername.empty(); }
    // Ключ AES-256-GCM для шифруемых полей БД (TOTP-секреты, токен ссылки управления):
    // sha256(SELFVOY_2FA_KEY), без него — sha256(SELFVOY_JWT_SECRET). 32 байта.
    std::string dataKey() const;

    // Устаревшие имена AURA_*, которые fromEnv() прочитал вместо SELFVOY_*
    // (совместимость на один релиз; validate() выдаёт по ним одно предупреждение).
    std::vector<std::string> legacyEnvNames;

    static Config fromEnv();
    std::string describe() const;  // для стартового лога (без секретов)

    // Проверка безопасности перед стартом. false + error (по-русски) — сервер
    // не должен запускаться. В dev-режиме может сгенерировать случайный
    // jwtSecret (warnings получает предупреждения для лога).
    bool validate(std::string& error, std::vector<std::string>& warnings);

    // true — секрет JWT слабый: пустой, короче 32 байт, известный dev-секрет
    // или заглушка из шаблона (CHANGE-IN-PRODUCTION).
    static bool weakSecret(const std::string& secret);
};

// Значение переменной окружения name (SELFVOY_X) или fallback. Если SELFVOY_X не
// задана, читается устаревшая AURA_X (совместимость на один релиз); тогда её имя
// дописывается в legacyNames (если передан, без повторов).
std::string envOr(const char* name, const std::string& fallback, std::vector<std::string>* legacyNames = nullptr);

}  // namespace selfvoy
