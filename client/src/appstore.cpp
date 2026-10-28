// client/src/appstore.cpp
#include "appstore.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QVariant>

// Интеграции (этап 9): системный браузер для consent-экрана Google и
// loopback HTTP-сервер, который ловит OAuth-редирект.
#include <QDesktopServices>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

// Голосовой ввод (STT) и озвучка (TTS): платформенные модули Qt.
// В QML их нет — вся работа с микрофоном и синтезом речи живёт здесь, в C++,
// а QML обращается только к свойствам/сигналам/слотам AppStore.
#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QTextToSpeech>
#include <QVoice>

#include <algorithm>
#include <cmath>

#include "websocketclient.h"

namespace selfvoy {

namespace {
constexpr const char* kTokenKey = "selfvoy/token";
constexpr const char* kRefreshKey = "selfvoy/refresh_token";
constexpr const char* kServerKey = "selfvoy/server";
constexpr const char* kDeviceIdKey = "selfvoy/device_id";
constexpr const char* kPendingFocusKey = "selfvoy/pending_focus";
// Голосовой ввод и озвучка.
constexpr const char* kAutoSendVoiceKey = "selfvoy/voice/auto_send";
constexpr const char* kMicLanguageKey = "selfvoy/voice/language";
constexpr const char* kTtsEnabledKey = "selfvoy/tts/enabled";
constexpr const char* kTtsVoiceKey = "selfvoy/tts/voice";
constexpr const char* kTtsRateKey = "selfvoy/tts/rate";
constexpr const char* kTtsVolumeKey = "selfvoy/tts/volume";
constexpr const char* kTtsImportantOnlyKey = "selfvoy/tts/important_only";
// Параметры записи: 16 кГц, моно, 16 бит — формат, который понимает Whisper.
constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kBitsPerSample = 16;

// Преобразование JSON-массива строк в QVariantList (для резервных кодов).
QVariantList jsonToStringList(const QJsonArray& array) {
    QVariantList list;
    for (const auto& value : array) list.append(value.toString());
    return list;
}
// Текст для экрана входа по причине из события session.revoked.
QString revokedMessage(const QString& reason) {
    if (reason == QLatin1String("password_changed") || reason == QLatin1String("password_reset"))
        return QStringLiteral("Пароль изменён — войдите снова");
    if (reason == QLatin1String("account_disabled"))
        return QStringLiteral("Аккаунт заблокирован администратором");
    if (reason == QLatin1String("logout"))
        return QStringLiteral("Вы вышли из аккаунта на другом устройстве");
    return QStringLiteral("Сессия завершена — войдите снова");
}
}  // namespace

AppStore::~AppStore() = default;

AppStore::AppStore(QObject* parent)
    : QObject(parent), client_(std::make_unique<WebSocketClient>(this)) {
    connect(client_.get(), &WebSocketClient::stateChanged, this, [this](WebSocketClient::State state) {
        if (state == WebSocketClient::State::Connected) {
            setStatus(QStringLiteral("Соединение установлено"));
            if (!token_.isEmpty()) {
                // Токен уже был передан в рукопожатии — просто проверяем профиль.
                // Если access-токен истёк/отозван — пробуем обменять refresh.
                client_->sendRequest(QStringLiteral("auth.me"), {},
                                     [this](const QJsonObject& response, const QString& error) {
                                         if (error.isEmpty()) {
                                             applyAuth(response.value("payload").toObject());
                                         } else if (!refreshToken_.isEmpty()) {
                                             tryRefresh();
                                         }
                                     });
            }
            loadChats();
            loadPreferences();
            loadMemory();
            loadServerInfo();  // telegram_bot — есть и в ответе без входа
            reconnectDelayMs_ = 2000;
        } else if (state == WebSocketClient::State::Disconnected) {
            // Ответы на запросы в полёте уже не придут — счётчик privacy и флаг
            // сохранения страницы записи сбрасываем; после переподключения prefs.get
            // вернёт актуальные флаги.
            privacyPending_ = 0;
            if (bookingSaving_) {
                bookingSaving_ = false;
                emit bookingSavingChanged();
            }
            setStatus(QStringLiteral("Нет соединения с сервером"));
            scheduleReconnect();
        }
        emit connectedChanged();
    });
    connect(client_.get(), &WebSocketClient::eventReceived, this, &AppStore::handleEvent);
    connect(client_.get(), &WebSocketClient::errorOccurred, this, &AppStore::setError);
}

void AppStore::setServerUrl(const QUrl& url) {
    serverUrlText_ = url.toString();
    client_->setUrl(url);
}

bool AppStore::changeServerUrl(const QString& text) {
    const QString value = text.trimmed();
    const QUrl url(value);
    if (!url.isValid() || (url.scheme() != QLatin1String("ws") && url.scheme() != QLatin1String("wss"))
            || url.host().isEmpty()) {
        setError(QStringLiteral("Адрес сервера начинается с ws:// или wss://"));
        return false;
    }
    QSettings settings;
    settings.setValue(QLatin1String(kServerKey), value);
    setServerUrl(url);
    reconnectDelayMs_ = 2000;
    if (client_->state() == WebSocketClient::State::Disconnected) {
        client_->connectToServer();
    } else {
        // Текущее соединение закроется, обработчик Disconnected подключится к новому адресу.
        client_->disconnectFromServer();
    }
    setStatus(QStringLiteral("Подключаемся к %1").arg(value));
    emit connectedChanged();
    return true;
}

void AppStore::scheduleReconnect() {
    if (reconnectScheduled_) return;
    reconnectScheduled_ = true;
    const int delay = reconnectDelayMs_;
    reconnectDelayMs_ = qMin(reconnectDelayMs_ * 2, 15000);
    QTimer::singleShot(delay, this, [this]() {
        reconnectScheduled_ = false;
        if (client_->state() == WebSocketClient::State::Disconnected) client_->connectToServer();
    });
}

void AppStore::start() {
    QSettings settings;
    token_ = settings.value(QLatin1String(kTokenKey)).toString();
    refreshToken_ = settings.value(QLatin1String(kRefreshKey)).toString();
    // Сохранённый токен уходит в рукопожатие: без него сессия неавторизована,
    // и первые запросы (prefs.get, chat.list) получают отказ.
    client_->setToken(token_);
    // Стабильный идентификатор устройства — для доверенных устройств 2FA.
    deviceId_ = settings.value(QLatin1String(kDeviceIdKey)).toString();
    if (deviceId_.isEmpty()) {
        deviceId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(QLatin1String(kDeviceIdKey), deviceId_);
    }
    if (serverUrlText_.isEmpty()) {
        serverUrlText_ = settings.value(QLatin1String(kServerKey), QStringLiteral("ws://127.0.0.1:9000")).toString();
        client_->setUrl(QUrl(serverUrlText_));
    }
    loadVoiceSettings();  // автоотправка голоса, язык, настройки TTS
    client_->connectToServer();
}

bool AppStore::connected() const { return client_->isConnected(); }

QString AppStore::serverUrl() const { return serverUrlText_; }

QString AppStore::connectionLabel() const {
    if (client_->state() == WebSocketClient::State::Connecting) {
        return QStringLiteral("подключение к %1…").arg(serverUrlText_);
    }
    if (!client_->isConnected()) {
        return QStringLiteral("нет связи с %1 — пробуем снова, проверьте адрес сервера").arg(serverUrlText_);
    }
    return authenticated() ? QStringLiteral("Selfvoy на связи") : QStringLiteral("сервер: %1").arg(serverUrlText_);
}

void AppStore::setBusy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    emit busyChanged();
}

void AppStore::setStatus(const QString& message) {
    if (statusMessage_ == message) return;
    statusMessage_ = message;
    emit statusMessageChanged();
}

void AppStore::setError(const QString& message) {
    if (message.isEmpty()) return;
    errorMessage_ = explainError(message);
    emit errorMessageChanged();
}

void AppStore::applyAuth(const QJsonObject& payload) {
    // auth.login/refresh/token кладут пользователя в поле user,
    // а auth.me возвращает его прямо в payload.
    const QJsonObject user = payload.contains(QStringLiteral("user"))
                                 ? payload.value(QStringLiteral("user")).toObject()
                                 : payload;
    const bool firstTime = userId_ == 0;

    storeTokens(payload);
    // Успешная авторизация гасит ожидания кодов.
    if (!pendingEmail_.isEmpty() || !devEmailCode_.isEmpty() || !devResetCode_.isEmpty()) {
        pendingEmail_.clear();
        devEmailCode_.clear();
        devResetCode_.clear();
        emit authFlowChanged();
    }
    if (user.contains(QStringLiteral("id"))) {
        userId_ = static_cast<qint64>(user.value(QStringLiteral("id")).toDouble());
    }
    userName_ = user.value(QStringLiteral("display_name")).toString();
    userEmail_ = user.value(QStringLiteral("email")).toString();
    userBio_ = user.value(QStringLiteral("bio")).toString();
    // Тариф (F3): {name: pro|free, until, enforced}; старый сервер поля не шлёт.
    plan_ = user.value(QStringLiteral("plan")).toObject().toVariantMap();

    if (payload.contains(QStringLiteral("preferences"))) {
        preferences_ = payload.value(QStringLiteral("preferences")).toObject().toVariantMap();
        emit preferencesChanged();
    }

    emit profileChanged();
    if (firstTime) emit authenticatedChanged();
    // Ответ из регистрации «С чем помогать?» — в память Selfvoy, один раз.
    {
        QSettings settings;
        const QString focus = settings.value(QLatin1String(kPendingFocusKey)).toString();
        if (!focus.isEmpty() && userId_ != 0) {
            settings.remove(QLatin1String(kPendingFocusKey));
            QJsonObject memory;
            memory.insert(QStringLiteral("text"), QStringLiteral("Просит помогать с: %1").arg(focus));
            memory.insert(QStringLiteral("kind"), QStringLiteral("preference"));
            client_->sendRequest(QStringLiteral("memory.add"), memory,
                                 [this](const QJsonObject&, const QString& error) {
                                     if (error.isEmpty()) loadMemory();
                                 });
        }
    }
    // Этап 8: сразу подтягиваем разрешения, отложенные подтверждения и задачи.
    loadPermissions();
    loadConfirmations();
    loadTasks();
    loadIntegrations();  // этап 9
    loadNotifications(); // этап 13
    loadPushDevices();   // этап 13
    setStatus(QStringLiteral("Добро пожаловать, %1").arg(userName_));
}

void AppStore::storeTokens(const QJsonObject& payload) {
    if (!payload.value(QStringLiteral("token")).toString().isEmpty()) {
        token_ = payload.value(QStringLiteral("token")).toString();
        QSettings settings;
        settings.setValue(QLatin1String(kTokenKey), token_);
        client_->setToken(token_);
    }
    const QString refresh = payload.value(QStringLiteral("refresh_token")).toString();
    if (!refresh.isEmpty()) {
        refreshToken_ = refresh;
        QSettings settings;
        settings.setValue(QLatin1String(kRefreshKey), refreshToken_);
    }
}

void AppStore::clearSession() {
    QSettings settings;
    settings.remove(QLatin1String(kTokenKey));
    settings.remove(QLatin1String(kRefreshKey));
    token_.clear();
    refreshToken_.clear();
    client_->setToken(QString());
    pendingEmail_.clear();
    devEmailCode_.clear();
    devResetCode_.clear();
    sessions_.clear();
    emit authFlowChanged();
    emit sessionsChanged();
    // 2FA-состояние тоже сбрасывается.
    twoFactorRequired_ = false;
    twoFactorEnabled_ = false;
    twoFactorPending_ = false;
    twoFactorSecret_.clear();
    twoFactorUri_.clear();
    recoveryCodes_.clear();
    recoveryCodesLeft_ = 0;
    trustedDevices_.clear();
    pendingLoginEmail_.clear();
    pendingLoginPassword_.clear();
    emit twoFactorRequiredChanged();
    emit twoFactorStatusChanged();
    emit twoFactorSetupChanged();
    emit trustedDevicesChanged();
    userId_ = 0;
    userName_.clear();
    userEmail_.clear();
    userBio_.clear();
    privacyPending_ = 0;
    bookingPages_.clear();
    guestBookings_.clear();
    bookingLoaded_ = false;
    referral_.clear();
    emit bookingChanged();
    emit referralChanged();
    // 2026 (F1–F6): тариф, абонементы, оплата, Telegram, календарь, пост.
    plan_.clear();
    freeSlots_.clear();
    packages_.clear();
    packagesLoaded_ = false;
    paymentsStatus_.clear();
    paymentsBusy_ = false;
    telegramStatus_.clear();
    calendarFeed_.clear();
    slotsPost_.clear();
    emit freeSlotsChanged();
    emit packagesChanged();
    emit paymentsChanged();
    emit telegramChanged();
    emit calendarFeedChanged();
    emit slotsPostChanged();
    chats_.clear();
    messages_.clear();
    memory_.clear();
    preferences_.clear();
    currentChatId_ = 0;
    chatTitle_.clear();
    emit chatsChanged();
    emit messagesChanged();
    emit memoryChanged();
    emit preferencesChanged();
    emit chatChanged();
    emit profileChanged();
    emit authenticatedChanged();
}

void AppStore::login(const QString& email, const QString& password) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), email);
    payload.insert(QStringLiteral("password"), password);
    payload.insert(QStringLiteral("device"), QStringLiteral("qt-client"));
    payload.insert(QStringLiteral("device_id"), deviceId_);

    client_->sendRequest(QStringLiteral("auth.login"), payload,
                         [this, email, password](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 // email_not_verified — не тупик: показываем ввод кода.
                                 if (error.startsWith(QStringLiteral("email_not_verified"))) {
                                     pendingEmail_ = email;
                                     devEmailCode_.clear();
                                     emit authFlowChanged();
                                     emit verificationRequested();
                                     setStatus(QStringLiteral("Введите код подтверждения, отправленный на %1").arg(email));
                                 } else if (error.startsWith(QStringLiteral("requires_2fa"))) {
                                     // Пароль верный, но нужна 2FA: запоминаем данные входа.
                                     pendingLoginEmail_ = email;
                                     pendingLoginPassword_ = password;
                                     twoFactorRequired_ = true;
                                     emit twoFactorRequiredChanged();
                                     setStatus(QStringLiteral("Введите код из приложения-аутентификатора"));
                                 } else {
                                     setError(error);
                                 }
                                 return;
                             }
                             applyAuth(response.value(QStringLiteral("payload")).toObject());
                             loadChats();
                             loadPreferences();
                             loadMemory();
                             loadTwoFactorStatus();
                         });
}

void AppStore::login2fa(const QString& code, bool trustDevice) {
    if (pendingLoginEmail_.isEmpty()) return;
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), pendingLoginEmail_);
    payload.insert(QStringLiteral("password"), pendingLoginPassword_);
    payload.insert(QStringLiteral("code"), code);
    payload.insert(QStringLiteral("trust_device"), trustDevice);
    payload.insert(QStringLiteral("device_id"), deviceId_);
    payload.insert(QStringLiteral("device"), QStringLiteral("qt-client"));

    client_->sendRequest(QStringLiteral("auth.login2fa"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             // Успех: гасим шаг 2FA и применяем сессию.
                             pendingLoginEmail_.clear();
                             pendingLoginPassword_.clear();
                             twoFactorRequired_ = false;
                             emit twoFactorRequiredChanged();
                             applyAuth(response.value(QStringLiteral("payload")).toObject());
                             loadChats();
                             loadPreferences();
                             loadMemory();
                             loadTwoFactorStatus();
                         });
}

void AppStore::cancelTwoFactor() {
    pendingLoginEmail_.clear();
    pendingLoginPassword_.clear();
    if (twoFactorRequired_) {
        twoFactorRequired_ = false;
        emit twoFactorRequiredChanged();
    }
    setStatus(QStringLiteral("Вход отменён"));
}

QString AppStore::normalizeReferralCode(const QString& raw) const {
    const QString code = raw.trimmed().toUpper();
    if (code.size() != 8) return QString();
    for (const QChar ch : code) {
        const bool ok = (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'));
        if (!ok) return QString();
    }
    return code;
}

void AppStore::registerUser(const QString& displayName, const QString& email, const QString& password,
                            const QString& ref) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("display_name"), displayName);
    payload.insert(QStringLiteral("email"), email);
    payload.insert(QStringLiteral("password"), password);
    payload.insert(QStringLiteral("device"), QStringLiteral("qt-client"));
    // Код приглашения: неверный не отправляем (сервер его всё равно молча проигнорирует).
    const QString code = normalizeReferralCode(ref);
    if (!code.isEmpty()) {
        payload.insert(QStringLiteral("ref"), code);
        payload.insert(QStringLiteral("ref_source"), QStringLiteral("link"));
    }

    client_->sendRequest(QStringLiteral("auth.register"), payload,
                         [this, email](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             if (body.value(QStringLiteral("requires_verification")).toBool()) {
                                 // Сессия ещё не выдаётся: ждём код подтверждения.
                                 pendingEmail_ = email;
                                 devEmailCode_ = body.value(QStringLiteral("email_code")).toString();
                                 emit authFlowChanged();
                                 emit verificationRequested();
                                 setStatus(QStringLiteral("Код подтверждения отправлен на %1").arg(email));
                                 return;
                             }
                             applyAuth(body);
                             loadChats();
                             loadPreferences();
                             loadMemory();
                         });
}

void AppStore::logout() {
    client_->sendRequest(QStringLiteral("auth.logout"), {}, nullptr);
    clearSession();
    setStatus(QStringLiteral("Вы вышли из аккаунта"));
}

// ---------------------------------------------------------------- auth v3
void AppStore::tryRefresh() {
    if (refreshToken_.isEmpty()) return;
    QJsonObject payload;
    payload.insert(QStringLiteral("refresh_token"), refreshToken_);
    payload.insert(QStringLiteral("device"), QStringLiteral("qt-client"));
    client_->sendRequest(QStringLiteral("auth.refresh"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 // Refresh не сработал (истёк/отозван) — просим вход заново.
                                 clearSession();
                                 setStatus(QStringLiteral("Сессия истекла — войдите снова"));
                                 return;
                             }
                             applyAuth(response.value(QStringLiteral("payload")).toObject());
                             client_->setToken(token_);
                             // Запросы, отправленные до продления, получили отказ — повторяем.
                             loadChats();
                             loadPreferences();
                             loadMemory();
                             setStatus(QStringLiteral("Сессия продлена"));
                         });
}

void AppStore::verifyEmail(const QString& email, const QString& code) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), email);
    payload.insert(QStringLiteral("code"), code);
    client_->sendRequest(QStringLiteral("auth.verifyEmail"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             pendingEmail_.clear();
                             devEmailCode_.clear();
                             emit authFlowChanged();
                             setStatus(QStringLiteral("Email подтверждён — теперь можно войти"));
                         });
}

void AppStore::resendCode(const QString& email) {
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), email);
    client_->sendRequest(QStringLiteral("auth.resendCode"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             const QString code = body.value(QStringLiteral("email_code")).toString();
                             if (!code.isEmpty()) {
                                 devEmailCode_ = code;
                                 emit authFlowChanged();
                             }
                             setStatus(QStringLiteral("Код отправлен повторно"));
                         });
}

void AppStore::forgotPassword(const QString& email) {
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), email);
    client_->sendRequest(QStringLiteral("auth.forgotPassword"), payload,
                         [this, email](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             pendingEmail_ = email;
                             devResetCode_ = body.value(QStringLiteral("reset_code")).toString();
                             emit authFlowChanged();
                             emit resetFlowRequested();
                             setStatus(QStringLiteral("Если email зарегистрирован, код сброса отправлен"));
                         });
}

void AppStore::resetPassword(const QString& email, const QString& code, const QString& newPassword) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("email"), email);
    payload.insert(QStringLiteral("code"), code);
    payload.insert(QStringLiteral("new_password"), newPassword);
    client_->sendRequest(QStringLiteral("auth.resetPassword"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             pendingEmail_.clear();
                             devResetCode_.clear();
                             emit authFlowChanged();
                             setStatus(QStringLiteral("Пароль изменён — войдите с новым паролем"));
                         });
}

void AppStore::changePassword(const QString& oldPassword, const QString& newPassword) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("old_password"), oldPassword);
    payload.insert(QStringLiteral("new_password"), newPassword);
    client_->sendRequest(QStringLiteral("auth.changePassword"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             // Сервер отозвал все сессии и refresh-токены, включая наши, и выдал
                             // этому устройству новую пару — без неё следующий вход по токену
                             // (переподключение, перезапуск) выкинул бы на экран входа.
                             const QJsonObject tokens = response.value(QStringLiteral("payload")).toObject();
                             // Старый refresh-токен сервер уже отозвал: если новый не пришёл,
                             // не храним мёртвый (иначе следующий refresh выкинет на вход).
                             if (tokens.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
                                 refreshToken_.clear();
                                 QSettings().remove(QLatin1String(kRefreshKey));
                             }
                             storeTokens(tokens);
                             setStatus(QStringLiteral("Пароль изменён, прочие устройства отключены"));
                         });
}

void AppStore::updateProfile(const QString& displayName, const QString& bio) {
    // Те же ограничения, что на сервере: имя 1–80 символов, «о себе» до 300
    // (считаем кодовые точки, как сервер, а не UTF-16).
    const QString name = displayName.trimmed();
    const QString about = bio.trimmed();
    QString problem;
    if (name.isEmpty() || name.toUcs4().size() > 80) {
        problem = QStringLiteral("Имя — от 1 до 80 символов");
    } else if (about.toUcs4().size() > 300) {
        problem = QStringLiteral("«О себе» — до 300 символов");
    }
    if (!problem.isEmpty()) {
        setError(problem);
        emit profileSaveFailed(problem);
        return;
    }
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("display_name"), name);
    payload.insert(QStringLiteral("bio"), about);
    client_->sendRequest(QStringLiteral("profile.update"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 emit profileSaveFailed(error);
                                 return;
                             }
                             const QJsonObject user = response.value(QStringLiteral("payload"))
                                                          .toObject()
                                                          .value(QStringLiteral("user"))
                                                          .toObject();
                             if (user.contains(QStringLiteral("display_name"))) {
                                 userName_ = user.value(QStringLiteral("display_name")).toString();
                             }
                             if (user.contains(QStringLiteral("email"))) {
                                 userEmail_ = user.value(QStringLiteral("email")).toString();
                             }
                             userBio_ = user.value(QStringLiteral("bio")).toString();
                             emit profileChanged();
                             emit profileSaved();
                             setStatus(QStringLiteral("Профиль сохранён"));
                         });
}

void AppStore::loadSessions() {
    client_->sendRequest(QStringLiteral("sessions.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             sessions_ = response.value(QStringLiteral("payload")).toObject()
                                             .value(QStringLiteral("sessions")).toArray().toVariantList();
                             emit sessionsChanged();
                         });
}

void AppStore::revokeSession(const QString& sessionId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("session_id"), sessionId);
    client_->sendRequest(QStringLiteral("sessions.revoke"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             loadSessions();
                         });
}

void AppStore::revokeAllSessions() {
    client_->sendRequest(QStringLiteral("sessions.revokeAll"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Прочие сессии завершены"));
                             loadSessions();
                         });
}

void AppStore::setup2fa() {
    setBusy(true);
    client_->sendRequest(QStringLiteral("auth.setup2fa"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             twoFactorSecret_ = body.value(QStringLiteral("secret")).toString();
                             twoFactorUri_ = body.value(QStringLiteral("otpauth_uri")).toString();
                             recoveryCodes_.clear();
                             twoFactorPending_ = true;
                             emit twoFactorSetupChanged();
                             emit twoFactorStatusChanged();
                             setStatus(QStringLiteral("Добавьте секрет в приложение-аутентификатор и введите код"));
                         });
}

void AppStore::confirm2fa(const QString& code) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("code"), code);
    client_->sendRequest(QStringLiteral("auth.confirm2fa"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             recoveryCodes_ = jsonToStringList(body.value(QStringLiteral("recovery_codes")).toArray());
                             // Секрет больше не нужен для показа — 2FA включена.
                             twoFactorSecret_.clear();
                             twoFactorUri_.clear();
                             twoFactorPending_ = false;
                             twoFactorEnabled_ = true;
                             emit twoFactorSetupChanged();
                             emit twoFactorStatusChanged();
                             setStatus(QStringLiteral("Двухфакторная аутентификация включена. Сохраните резервные коды!"));
                         });
}

void AppStore::disable2fa(const QString& password) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("password"), password);
    client_->sendRequest(QStringLiteral("auth.disable2fa"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             twoFactorEnabled_ = false;
                             twoFactorPending_ = false;
                             recoveryCodes_.clear();
                             recoveryCodesLeft_ = 0;
                             trustedDevices_.clear();
                             emit twoFactorSetupChanged();
                             emit twoFactorStatusChanged();
                             emit trustedDevicesChanged();
                             setStatus(QStringLiteral("Двухфакторная аутентификация отключена"));
                         });
}

void AppStore::loadTwoFactorStatus() {
    client_->sendRequest(QStringLiteral("auth.status2fa"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             twoFactorEnabled_ = body.value(QStringLiteral("enabled")).toBool();
                             twoFactorPending_ = body.value(QStringLiteral("pending")).toBool();
                             recoveryCodesLeft_ = body.value(QStringLiteral("recovery_codes_left")).toInt();
                             emit twoFactorStatusChanged();
                         });
}

void AppStore::loadTrustedDevices() {
    client_->sendRequest(QStringLiteral("devices.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             trustedDevices_ = body.value(QStringLiteral("devices")).toArray().toVariantList();
                             emit trustedDevicesChanged();
                         });
}

void AppStore::revokeTrustedDevice(const QString& id) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), id);
    client_->sendRequest(QStringLiteral("devices.revoke"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Доверенное устройство отозвано"));
                             loadTrustedDevices();
                         });
}

void AppStore::revokeAllTrustedDevices() {
    client_->sendRequest(QStringLiteral("devices.revokeAll"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Все доверенные устройства отозваны"));
                             loadTrustedDevices();
                         });
}

void AppStore::openChat(const QString& contact) {
    if (contact.trimmed().isEmpty()) {
        setError(QStringLiteral("Укажите email собеседника"));
        return;
    }
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("contact"), contact.trimmed());
    client_->sendRequest(QStringLiteral("chat.open"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject chat =
                                 response.value(QStringLiteral("payload")).toObject().value(QStringLiteral("chat")).toObject();
                             loadChats();
                             selectChat(static_cast<qint64>(chat.value(QStringLiteral("id")).toDouble()));
                         });
}

void AppStore::selectChat(qint64 chatId) {
    currentChatId_ = chatId;
    for (const QVariant& item : std::as_const(chats_)) {
        const QVariantMap chat = item.toMap();
        if (chat.value(QStringLiteral("id")).toLongLong() == chatId) {
            chatTitle_ = chat.value(QStringLiteral("title")).toString();
            if (chatTitle_.isEmpty()) {
                chatTitle_ = chat.value(QStringLiteral("last_message_sender")).toString();
            }
            break;
        }
    }
    if (chatTitle_.isEmpty()) chatTitle_ = QStringLiteral("Чат #%1").arg(chatId);
    emit chatChanged();
    loadMessages(chatId);
}

void AppStore::sendMessage(qint64 chatId, const QString& text) {
    if (text.trimmed().isEmpty()) return;
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("chat_id"), static_cast<double>(chatId));
    payload.insert(QStringLiteral("body"), text.trimmed());
    client_->sendRequest(QStringLiteral("chat.send"), payload,
                         [this, chatId](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             loadMessages(chatId);
                         });
}

void AppStore::askAgent(qint64 chatId, const QString& message) {
    if (message.trimmed().isEmpty()) {
        setError(QStringLiteral("Опишите, о чём договориться"));
        return;
    }
    setBusy(true);
    setStatus(QStringLiteral("Selfvoy думает…"));
    QJsonObject payload;
    payload.insert(QStringLiteral("chat_id"), static_cast<double>(chatId));
    payload.insert(QStringLiteral("message"), message.trimmed());
    payload.insert(QStringLiteral("execute"), true);

    client_->sendRequest(QStringLiteral("agent.ask"), payload,
                         [this, chatId](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             const QString reply = body.value(QStringLiteral("reply")).toString();
                             setStatus(reply.isEmpty() ? QStringLiteral("Selfvoy выполнил задачу") : reply);
                             // Ответ Selfvoy — «важное»: озвучиваем даже в режиме «только важное».
                             speakImportant(reply.isEmpty() ? QStringLiteral("Готово") : reply);
                             loadMessages(chatId);
                             loadMemory();
                             // Этап 8: опасные операции не исполнены — ждут подтверждения.
                             int pendingCount = 0;
                             const QJsonArray results = body.value(QStringLiteral("results")).toArray();
                             for (const QJsonValue& item : results) {
                                 if (item.toObject().value(QStringLiteral("requires_confirmation")).toBool()) {
                                     ++pendingCount;
                                 }
                             }
                             if (pendingCount > 0) {
                                 loadConfirmations();
                                 setStatus(QStringLiteral("Нужно подтверждение: %1 действи%2")
                                               .arg(pendingCount)
                                               .arg(pendingCount == 1 ? "е" : "я"));
                             }
                         });
}

void AppStore::askAgentGlobal(const QString& message) {
    if (message.trimmed().isEmpty()) {
        setError(QStringLiteral("Опишите, что сделать"));
        return;
    }
    setBusy(true);
    setStatus(QStringLiteral("Selfvoy думает…"));
    appendDialog({{QStringLiteral("from"), QStringLiteral("user")},
                  {QStringLiteral("text"), message.trimmed()}});
    QJsonObject payload;
    payload.insert(QStringLiteral("message"), message.trimmed());
    payload.insert(QStringLiteral("execute"), true);

    client_->sendRequest(QStringLiteral("agent.ask"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 appendDialog({{QStringLiteral("from"), QStringLiteral("selfvoy")},
                                               {QStringLiteral("text"), error},
                                               {QStringLiteral("error"), true}});
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             homeReply_ = body.value(QStringLiteral("reply")).toString();
                             emit homeReplyChanged();
                             // Ответ в диалог: опасные действия, ждущие подтверждения,
                             // становятся кнопками «Подтвердить / Отклонить» под репликой.
                             QVariantList actions;
                             for (const QJsonValue& item : body.value(QStringLiteral("results")).toArray()) {
                                 const QJsonObject result = item.toObject();
                                 if (!result.value(QStringLiteral("requires_confirmation")).toBool()) continue;
                                 actions.append(QVariantMap{
                                     {QStringLiteral("id"), result.value(QStringLiteral("confirmation_id")).toVariant()},
                                     {QStringLiteral("summary"), result.value(QStringLiteral("summary")).toString()},
                                     {QStringLiteral("tool"), result.value(QStringLiteral("tool")).toString()}});
                             }
                             QVariantMap entry{
                                 {QStringLiteral("from"), QStringLiteral("selfvoy")},
                                 {QStringLiteral("text"), homeReply_.isEmpty() ? QStringLiteral("Готово") : homeReply_},
                                 {QStringLiteral("intent"), body.value(QStringLiteral("intent")).toString()},
                                 {QStringLiteral("actions"), actions}};
                             const QJsonValue a2a = body.value(QStringLiteral("a2a"));
                             if (a2a.isObject()) entry.insert(QStringLiteral("a2a"), a2a.toObject().toVariantMap());
                             appendDialog(entry);
                             setStatus(homeReply_.isEmpty() ? QStringLiteral("Selfvoy выполнил задачу")
                                                            : homeReply_);
                             speakImportant(homeReply_.isEmpty() ? QStringLiteral("Готово") : homeReply_);
                             loadNotifications();
                             loadConfirmations();
                         });
}

void AppStore::negotiateAgent(const QString& target, const QString& topic,
                              int durationMinutes, int windowHours) {
    if (target.trimmed().isEmpty()) {
        setError(QStringLiteral("Укажите собеседника (email или имя)"));
        return;
    }
    setBusy(true);
    setStatus(QStringLiteral("Оба Selfvoy договариваются…"));
    QJsonObject payload;
    payload.insert(QStringLiteral("target"), target.trimmed());
    if (!topic.trimmed().isEmpty()) payload.insert(QStringLiteral("topic"), topic.trimmed());
    if (durationMinutes > 0) payload.insert(QStringLiteral("duration_minutes"), durationMinutes);
    if (windowHours > 0) payload.insert(QStringLiteral("window_hours"), windowHours);

    client_->sendRequest(QStringLiteral("agent.negotiate"), payload,
                         [this, payload](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             negotiation_ =
                                 response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             // Запрос рядом с ответом: экран показывает, с кем и о чём
                             // договаривались, и считает раунды с этим собеседником.
                             const QString target = payload.value(QStringLiteral("target")).toString();
                             int round = 1;
                             for (const QVariant& item : negotiationHistory_) {
                                 if (item.toMap().value(QStringLiteral("target")).toString() != target) break;
                                 ++round;
                             }
                             negotiation_.insert(QStringLiteral("target"), target);
                             negotiation_.insert(QStringLiteral("topic"), payload.value(QStringLiteral("topic")).toString());
                             negotiation_.insert(QStringLiteral("duration_minutes"),
                                                 payload.value(QStringLiteral("duration_minutes")).toInt(60));
                             negotiation_.insert(QStringLiteral("window_hours"),
                                                 payload.value(QStringLiteral("window_hours")).toInt(96));
                             negotiation_.insert(QStringLiteral("round"), round);
                             negotiation_.insert(QStringLiteral("at"),
                                                 QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
                             negotiationHistory_.prepend(negotiation_);
                             emit negotiationChanged();
                             const bool accepted = negotiation_.value(QStringLiteral("accepted")).toBool();
                             setStatus(accepted ? QStringLiteral("Оба Selfvoy договорились")
                                                : QStringLiteral("Собеседник занят — попробуйте позже"));
                             speakImportant(accepted ? QStringLiteral("Встреча согласована")
                                                     : QStringLiteral("Договориться не удалось"));
                         });
}

QVariantMap AppStore::privacy() const {
    // Сервер всегда отдаёт все три флага; значения по умолчанию — на случай
    // старого сервера или ещё не загруженных настроек.
    QVariantMap result{{QStringLiteral("share_free_slots_only"), true},
                       {QStringLiteral("hide_event_titles"), true},
                       {QStringLiteral("allow_place_choice"), true}};
    const QVariantMap stored = preferences_.value(QStringLiteral("privacy")).toMap();
    for (auto it = stored.cbegin(); it != stored.cend(); ++it) {
        if (result.contains(it.key()) && it.value().typeId() == QMetaType::Bool) {
            result.insert(it.key(), it.value());
        }
    }
    return result;
}

void AppStore::setPrivacy(const QString& key, bool value) {
    QVariantMap current = privacy();
    if (!current.contains(key)) {
        const QString problem = QStringLiteral("Неизвестная настройка приватности: %1").arg(key);
        setError(problem);
        emit privacySaveFailed(key, problem);
        return;
    }
    const bool previous = current.value(key).toBool();
    if (previous == value) return;
    // Оптимистично: переключатель меняется сразу, при ошибке — откат.
    current.insert(key, value);
    preferences_.insert(QStringLiteral("privacy"), current);
    emit preferencesChanged();
    ++privacyPending_;
    QJsonObject flags;
    flags.insert(key, value);
    QJsonObject payload;
    payload.insert(QStringLiteral("privacy"), flags);  // в корне payload, без обёртки
    client_->sendRequest(QStringLiteral("prefs.set"), payload,
                         [this, key, previous](const QJsonObject& response, const QString& error) {
                             privacyPending_ = qMax(0, privacyPending_ - 1);
                             if (!error.isEmpty()) {
                                 QVariantMap rolledBack = privacy();
                                 rolledBack.insert(key, previous);
                                 preferences_.insert(QStringLiteral("privacy"), rolledBack);
                                 emit preferencesChanged();
                                 setError(QStringLiteral("Настройка приватности не сохранена: %1").arg(error));
                                 emit privacySaveFailed(key, error);
                                 return;
                             }
                             // Пока летят другие переключения, ответ не затирает их значения.
                             if (privacyPending_ == 0) {
                                 preferences_ =
                                     response.value(QStringLiteral("payload")).toObject().toVariantMap();
                                 emit preferencesChanged();
                             }
                             setStatus(QStringLiteral("Приватность ИИ обновлена"));
                         });
}

void AppStore::savePreferences(const QVariantMap& preferences) {
    setBusy(true);
    client_->sendRequest(QStringLiteral("prefs.set"), QJsonObject::fromVariantMap(preferences),
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QVariant privacyNow = preferences_.value(QStringLiteral("privacy"));
                             preferences_ =
                                 response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             if (privacyPending_ > 0) preferences_.insert(QStringLiteral("privacy"), privacyNow);
                             emit preferencesChanged();
                             setStatus(QStringLiteral("Настройки Selfvoy сохранены"));
                         });
}

void AppStore::refreshMemory() { loadMemory(); }

void AppStore::loadChats() {
    client_->sendRequest(QStringLiteral("chat.list"), {}, [this](const QJsonObject& response, const QString& error) {
        if (!error.isEmpty()) {
            setError(error);
            return;
        }
        const QJsonArray list =
            response.value(QStringLiteral("payload")).toObject().value(QStringLiteral("chats")).toArray();
        chats_ = list.toVariantList();
        emit chatsChanged();
    });
}

void AppStore::loadMessages(qint64 chatId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("chat_id"), static_cast<double>(chatId));
    payload.insert(QStringLiteral("limit"), 100);
    client_->sendRequest(QStringLiteral("chat.history"), payload,
                         [this, chatId](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             if (chatId != currentChatId_) return;  // пользователь уже ушёл в другой чат
                             const QJsonArray list = body.value(QStringLiteral("messages")).toArray();
                             messages_ = list.toVariantList();
                             const QJsonObject chat = body.value(QStringLiteral("chat")).toObject();
                             if (!chat.isEmpty()) {
                                 const QString title = chat.value(QStringLiteral("title")).toString();
                                 if (!title.isEmpty()) {
                                     chatTitle_ = title;
                                     emit chatChanged();
                                 }
                             }
                             emit messagesChanged();
                         });
}

void AppStore::loadPreferences() {
    client_->sendRequest(QStringLiteral("prefs.get"), {}, [this](const QJsonObject& response, const QString& error) {
        if (!error.isEmpty()) return;
        const QVariant privacyNow = preferences_.value(QStringLiteral("privacy"));
        preferences_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
        if (privacyPending_ > 0) preferences_.insert(QStringLiteral("privacy"), privacyNow);
        emit preferencesChanged();
    });
}

void AppStore::loadMemory() {
    QJsonObject payload;
    payload.insert(QStringLiteral("limit"), 30);
    client_->sendRequest(QStringLiteral("memory.list"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             const QJsonArray list =
                                 response.value(QStringLiteral("payload")).toObject().value(QStringLiteral("entries")).toArray();
                             memory_ = list.toVariantList();
                             emit memoryChanged();
                         });
}

void AppStore::handleEvent(const QString& name, const QJsonObject& payload) {
    if (name == QLatin1String("session.revoked")) {
        // Сессию этого соединения отозвали (выход/смена пароля на другом устройстве,
        // блокировка) — сервер сейчас закроет сокет кодом 4401. Токены мертвы:
        // не ждём переподключения, сразу возвращаемся на экран входа.
        const QString reason = payload.value(QStringLiteral("reason")).toString();
        clearSession();
        setStatus(revokedMessage(reason));
        return;
    }
    if (name == QLatin1String("chat.message")) {
        const qint64 chatId = static_cast<qint64>(payload.value(QStringLiteral("chat_id")).toDouble());
        if (chatId == currentChatId_) {
            // Дописываем сообщение сразу — так диалог ощущается живым.
            messages_.append(payload.toVariantMap());
            emit messagesChanged();
        } else {
            const QString sender = payload.value(QStringLiteral("sender_name")).toString();
            setStatus(QStringLiteral("Новое сообщение от %1").arg(sender));
        }
        loadChats();
        return;
    }
    if (name == QLatin1String("chat.created")) {
        loadChats();
        setStatus(QStringLiteral("Вас добавили в новый чат"));
        return;
    }
    if (name == QLatin1String("task.due")) {
        // Этап 8: планировщик прислал напоминание — в начало списка и озвучить.
        tasks_.prepend(payload.toVariantMap());
        emit tasksChanged();
        const QString title = payload.value(QStringLiteral("title")).toString();
        setStatus(QStringLiteral("Напоминание: %1").arg(title));
        speakImportant(QStringLiteral("Напоминание: %1").arg(title));
        return;
    }
    if (name == QLatin1String("notification.new")) {
        // Этап 13: серверное уведомление — в начало «входящей» + счётчик.
        notifications_.prepend(payload.toVariantMap());
        ++unreadNotifications_;
        emit notificationsChanged();
        const QString kind = payload.value(QStringLiteral("kind")).toString();
        // booking.created/rescheduled/cancelled/confirmed/payment — записи и окна дня меняются.
        if (kind.startsWith(QLatin1String("booking."))) {
            if (bookingLoaded_) loadBooking();
            if (kind != QLatin1String("booking.confirmed") && kind != QLatin1String("booking.payment"))
                loadFreeSlots();
        }
        // package.purchased/payment/low — список абонементов (и записи по пакету).
        if (kind.startsWith(QLatin1String("package."))) {
            if (packagesLoaded_) loadPackages(packagesFilter_);
            if (kind == QLatin1String("package.purchased") && bookingLoaded_) loadBooking();
        }
        if ((kind == QLatin1String("referral.joined") || kind == QLatin1String("referral.reward"))
                && !referral_.isEmpty())
            loadReferral();
        // Месяц Pro в подарок — тариф в профиле обновится из auth.me.
        if (kind == QLatin1String("referral.reward") || kind == QLatin1String("referral.joined")) refreshProfile();
        setStatus(QStringLiteral("%1: %2")
                      .arg(payload.value(QStringLiteral("title")).toString(),
                           payload.value(QStringLiteral("body")).toString()));
        return;
    }
    if (name == QLatin1String("session.ready")) {
        setStatus(QStringLiteral("Сессия активна"));
    }
}

// ============================================================ Голосовой ввод (STT)
void AppStore::setVoiceState(const QString& state) {
    if (voiceState_ == state) return;
    voiceState_ = state;
    emit voiceStateChanged();
}

bool AppStore::ensureMicAvailable() {
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    const bool available = !device.isNull();
    if (available != micAvailable_) {
        micAvailable_ = available;
        emit micAvailableChanged();
    }
    return micAvailable_;
}

void AppStore::startListening() {
    if (voiceState_ == QLatin1String("listening")) return;
    if (!ensureMicAvailable()) {
        // Отказ микрофона — не тупик: текстовый фолбэк и понятная ошибка.
        setVoiceState(QStringLiteral("error"));
        setError(QStringLiteral("Микрофон недоступен — введите сообщение текстом"));
        return;
    }
    transcript_.clear();
    emit transcriptChanged();
    startCapture();
    setVoiceState(QStringLiteral("listening"));
    setStatus(QStringLiteral("Слушаю…"));
}

void AppStore::startCapture() {
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!device.isFormatSupported(format)) {
        // Берём предпочитаемый формат устройства — фактические параметры
        // возьмём из audioSource_->format() при упаковке в WAV.
        format = device.preferredFormat();
        format.setSampleFormat(QAudioFormat::Int16);
    }

    audioSource_ = new QAudioSource(device, format, this);
    recordedPcm_.clear();
    voiceLevel_ = 0.0;
    emit voiceLevelChanged();
    audioInput_ = audioSource_->start();
    if (audioInput_) {
        connect(audioInput_, &QIODevice::readyRead, this, &AppStore::onAudioReady);
    }
}

void AppStore::onAudioReady() {
    if (!audioInput_) return;
    const QByteArray chunk = audioInput_->readAll();
    if (chunk.isEmpty()) return;
    recordedPcm_.append(chunk);

    // Уровень (RMS) для индикатора/волны.
    const qint16* samples = reinterpret_cast<const qint16*>(chunk.constData());
    const int count = chunk.size() / static_cast<int>(sizeof(qint16));
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
        const double s = static_cast<double>(samples[i]) / 32768.0;
        sum += s * s;
    }
    const double rms = count > 0 ? std::sqrt(sum / count) : 0.0;
    const qreal level = qBound(0.0, rms * 3.0, 1.0);
    if (std::abs(level - voiceLevel_) > 0.01) {
        voiceLevel_ = level;
        emit voiceLevelChanged();
    }
}

void AppStore::stopListening() {
    if (voiceState_ != QLatin1String("listening")) return;
    stopCaptureAndTranscribe();
}

void AppStore::stopCaptureAndTranscribe() {
    QAudioFormat format;
    if (audioSource_) {
        format = audioSource_->format();
        audioSource_->stop();
        audioSource_->deleteLater();
        audioSource_ = nullptr;
    }
    audioInput_ = nullptr;
    voiceLevel_ = 0.0;
    emit voiceLevelChanged();

    if (recordedPcm_.isEmpty()) {
        setVoiceState(QStringLiteral("error"));
        setError(QStringLiteral("Не удалось записать звук — попробуйте ещё раз"));
        return;
    }

    const int sampleRate = format.sampleRate() > 0 ? format.sampleRate() : kSampleRate;
    const int channels = format.channelCount() > 0 ? format.channelCount() : kChannels;
    const QByteArray wav = wrapWav(recordedPcm_, sampleRate, channels, kBitsPerSample);
    recordedPcm_.clear();

    setVoiceState(QStringLiteral("processing"));
    setStatus(QStringLiteral("Распознаю речь…"));

    QJsonObject payload;
    payload.insert(QStringLiteral("audio"), QString::fromLatin1(wav.toBase64()));
    payload.insert(QStringLiteral("language"), micLanguage_);
    payload.insert(QStringLiteral("format"), QStringLiteral("wav"));

    client_->sendRequest(QStringLiteral("speech.transcribe"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setVoiceState(QStringLiteral("error"));
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             transcript_ = body.value(QStringLiteral("text")).toString();
                             emit transcriptChanged();
                             setVoiceState(QStringLiteral("success"));
                             if (autoSendVoice_ && !transcript_.trimmed().isEmpty()) {
                                 sendTranscript();
                             } else {
                                 setStatus(QStringLiteral("Проверьте текст и отправьте"));
                             }
                         });
}

void AppStore::cancelListening() {
    if (audioSource_) {
        audioSource_->stop();
        audioSource_->deleteLater();
        audioSource_ = nullptr;
    }
    audioInput_ = nullptr;
    recordedPcm_.clear();
    voiceLevel_ = 0.0;
    emit voiceLevelChanged();
    setVoiceState(QStringLiteral("idle"));
    setStatus(QStringLiteral("Запись отменена"));
}

void AppStore::setTranscript(const QString& text) {
    if (transcript_ == text) return;
    transcript_ = text;
    emit transcriptChanged();
}

void AppStore::sendTranscript() {
    const QString text = transcript_.trimmed();
    if (text.isEmpty()) {
        setVoiceState(QStringLiteral("idle"));
        return;
    }
    if (currentChatId_ == 0) {
        setError(QStringLiteral("Откройте чат, чтобы отправить сообщение"));
        return;
    }
    sendMessage(currentChatId_, text);
    transcript_.clear();
    emit transcriptChanged();
    setVoiceState(QStringLiteral("idle"));
}

void AppStore::setAutoSendVoice(bool enabled) {
    if (autoSendVoice_ == enabled) return;
    autoSendVoice_ = enabled;
    saveVoiceSettings();
    emit autoSendVoiceChanged();
}

void AppStore::setMicLanguage(const QString& lang) {
    if (micLanguage_ == lang) return;
    micLanguage_ = lang;
    saveVoiceSettings();
    emit micLanguageChanged();
}

QByteArray AppStore::wrapWav(const QByteArray& pcm, int sampleRate, int channels, int bitsPerSample) {
    auto put32 = [](QByteArray& out, quint32 v) {
        out.append(static_cast<char>(v & 0xFF));
        out.append(static_cast<char>((v >> 8) & 0xFF));
        out.append(static_cast<char>((v >> 16) & 0xFF));
        out.append(static_cast<char>((v >> 24) & 0xFF));
    };
    auto put16 = [](QByteArray& out, quint16 v) {
        out.append(static_cast<char>(v & 0xFF));
        out.append(static_cast<char>((v >> 8) & 0xFF));
    };
    const int byteRate = sampleRate * channels * bitsPerSample / 8;
    const int blockAlign = channels * bitsPerSample / 8;

    QByteArray wav;
    wav.reserve(44 + pcm.size());
    wav.append("RIFF", 4);
    put32(wav, static_cast<quint32>(36 + pcm.size()));
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    put32(wav, 16);                                   // размер fmt-чанка (PCM)
    put16(wav, 1);                                    // формат = PCM
    put16(wav, static_cast<quint16>(channels));
    put32(wav, static_cast<quint32>(sampleRate));
    put32(wav, static_cast<quint32>(byteRate));
    put16(wav, static_cast<quint16>(blockAlign));
    put16(wav, static_cast<quint16>(bitsPerSample));
    wav.append("data", 4);
    put32(wav, static_cast<quint32>(pcm.size()));
    wav.append(pcm);
    return wav;
}

// ============================================================ Озвучка (TTS)
void AppStore::initTts() {
    if (tts_) return;
    tts_ = new QTextToSpeech(this);
    connect(tts_, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State state) {
        const bool nowSpeaking = (state == QTextToSpeech::State::Speaking);
        if (nowSpeaking != speaking_) {
            speaking_ = nowSpeaking;
            emit speakingChanged();
        }
    });
    ttsVoices_.clear();
    const auto voices = tts_->availableVoices();
    for (const QVoice& voice : voices) ttsVoices_.append(voice.name());
    applyTtsSettings();
    emit ttsChanged();
}

void AppStore::applyTtsSettings() {
    if (!tts_) return;
    tts_->setRate(static_cast<float>(ttsRate_));
    tts_->setVolume(static_cast<float>(ttsVolume_));
    if (!ttsVoice_.isEmpty()) {
        const auto voices = tts_->availableVoices();
        for (const QVoice& voice : voices) {
            if (voice.name() == ttsVoice_) {
                tts_->setVoice(voice);
                break;
            }
        }
    }
}

void AppStore::speak(const QString& text) {
    if (!ttsEnabled_ || ttsImportantOnly_ || text.trimmed().isEmpty()) return;
    if (!tts_) initTts();
    if (tts_) tts_->say(text);
}

void AppStore::speakImportant(const QString& text) {
    if (!ttsEnabled_ || text.trimmed().isEmpty()) return;
    if (!tts_) initTts();
    if (tts_) tts_->say(text);
}

void AppStore::stopSpeaking() {
    if (tts_) tts_->stop();
}

void AppStore::setTtsEnabled(bool enabled) {
    if (ttsEnabled_ == enabled) return;
    ttsEnabled_ = enabled;
    if (enabled && !tts_) initTts();
    saveVoiceSettings();
    emit ttsChanged();
}

void AppStore::setTtsVoice(const QString& voice) {
    if (ttsVoice_ == voice) return;
    ttsVoice_ = voice;
    applyTtsSettings();
    saveVoiceSettings();
    emit ttsChanged();
}

void AppStore::setTtsRate(qreal rate) {
    rate = qBound(-1.0, rate, 1.0);
    if (std::abs(ttsRate_ - rate) < 1e-9) return;
    ttsRate_ = rate;
    applyTtsSettings();
    saveVoiceSettings();
    emit ttsChanged();
}

void AppStore::setTtsVolume(qreal volume) {
    volume = qBound(0.0, volume, 1.0);
    if (std::abs(ttsVolume_ - volume) < 1e-9) return;
    ttsVolume_ = volume;
    applyTtsSettings();
    saveVoiceSettings();
    emit ttsChanged();
}

void AppStore::setTtsImportantOnly(bool enabled) {
    if (ttsImportantOnly_ == enabled) return;
    ttsImportantOnly_ = enabled;
    saveVoiceSettings();
    emit ttsChanged();
}

void AppStore::loadVoiceSettings() {
    QSettings settings;
    autoSendVoice_ = settings.value(QLatin1String(kAutoSendVoiceKey), false).toBool();
    micLanguage_ = settings.value(QLatin1String(kMicLanguageKey), QStringLiteral("auto")).toString();
    ttsEnabled_ = settings.value(QLatin1String(kTtsEnabledKey), false).toBool();
    ttsVoice_ = settings.value(QLatin1String(kTtsVoiceKey)).toString();
    ttsRate_ = settings.value(QLatin1String(kTtsRateKey), 0.0).toDouble();
    ttsVolume_ = settings.value(QLatin1String(kTtsVolumeKey), 1.0).toDouble();
    ttsImportantOnly_ = settings.value(QLatin1String(kTtsImportantOnlyKey), false).toBool();
    initTts();
    emit autoSendVoiceChanged();
    emit ttsChanged();
}

void AppStore::saveVoiceSettings() {
    QSettings settings;
    settings.setValue(QLatin1String(kAutoSendVoiceKey), autoSendVoice_);
    settings.setValue(QLatin1String(kMicLanguageKey), micLanguage_);
    settings.setValue(QLatin1String(kTtsEnabledKey), ttsEnabled_);
    settings.setValue(QLatin1String(kTtsVoiceKey), ttsVoice_);
    settings.setValue(QLatin1String(kTtsRateKey), ttsRate_);
    settings.setValue(QLatin1String(kTtsVolumeKey), ttsVolume_);
    settings.setValue(QLatin1String(kTtsImportantOnlyKey), ttsImportantOnly_);
}

// ---------------------------------------------------------------------------
//  Этап 8: разрешения, подтверждения, задачи
// ---------------------------------------------------------------------------

// ---------------------------------------------------------- страницы записи
namespace {

// Те же пределы, что проверяет сервер (BookingManager::pageCreate); "" — всё верно.
QString bookingFieldsProblem(const QVariantMap& fields) {
    const QString title = fields.value(QStringLiteral("title")).toString().trimmed();
    const auto inRange = [&fields](const char* key, int min, int max) {
        bool ok = false;
        const int value = fields.value(QLatin1String(key)).toInt(&ok);
        return ok && value >= min && value <= max;
    };
    if (title.isEmpty()) return QStringLiteral("Назовите страницу — например, «Консультация»");
    if (title.toUcs4().size() > 120 || title.contains(QLatin1Char('\n')))
        return QStringLiteral("Название — до 120 символов в одну строку");
    if (fields.value(QStringLiteral("description")).toString().trimmed().toUcs4().size() > 1000)
        return QStringLiteral("Описание — до 1000 символов");
    if (!inRange("duration_minutes", 15, 240)) return QStringLiteral("Длительность — от 15 до 240 минут");
    if (!inRange("days_ahead", 1, 60)) return QStringLiteral("Запись вперёд — от 1 до 60 дней");
    if (!inRange("max_per_day", 1, 16)) return QStringLiteral("Встреч в день — от 1 до 16");
    if (!inRange("notice_hours", 0, 168)) return QStringLiteral("Запас до встречи — от 0 до 168 часов");
    if (!inRange("max_series", 1, 12)) return QStringLiteral("Серия — от 1 до 12 встреч");
    // F4: цена, предоплата, политика отмены (проверяются, только если переданы).
    const auto has = [&fields](const char* key) { return fields.contains(QLatin1String(key)); };
    const qint64 price = fields.value(QStringLiteral("price")).toLongLong();
    if (has("price") && (price < 0 || price > 100000000))
        return QStringLiteral("Цена — от 0 до 1 000 000");
    if (has("currency")) {
        static const QStringList currencies = {QStringLiteral("RUB"), QStringLiteral("USD"), QStringLiteral("EUR"),
                                               QStringLiteral("KZT"), QStringLiteral("BYN")};
        if (!currencies.contains(fields.value(QStringLiteral("currency")).toString()))
            return QStringLiteral("Валюта — рубли, доллары, евро, тенге или белорусские рубли");
    }
    const QString prepay = fields.value(QStringLiteral("prepay"), QStringLiteral("none")).toString();
    if (prepay != QLatin1String("none") && prepay != QLatin1String("full") && prepay != QLatin1String("deposit"))
        return QStringLiteral("Предоплата — нет, полная или депозит");
    if (prepay != QLatin1String("none") && price <= 0)
        return QStringLiteral("Для предоплаты укажите цену");
    if (prepay == QLatin1String("deposit")) {
        const qint64 deposit = fields.value(QStringLiteral("deposit")).toLongLong();
        if (deposit < 1 || deposit > price) return QStringLiteral("Депозит — больше нуля и не больше цены");
    }
    const QString paymentUrl = fields.value(QStringLiteral("payment_url")).toString().trimmed();
    if (!paymentUrl.isEmpty() && (!paymentUrl.startsWith(QLatin1String("https://")) || paymentUrl.contains(QLatin1Char(' '))
                                  || paymentUrl.size() > 500))
        return QStringLiteral("Ссылка оплаты — только https://, без пробелов, до 500 символов");
    const QString paymentNote = fields.value(QStringLiteral("payment_note")).toString().trimmed();
    if (paymentNote.toUcs4().size() > 300 || paymentNote.contains(QLatin1Char('\n')))
        return QStringLiteral("Реквизиты — одна строка до 300 символов");
    if (has("pay_within_minutes") && !inRange("pay_within_minutes", 15, 1440))
        return QStringLiteral("Срок оплаты — от 15 до 1440 минут");
    if (has("cancel_hours") && !inRange("cancel_hours", 0, 168))
        return QStringLiteral("Бесплатная отмена — от 0 до 168 часов");
    if (has("late_cancel")) {
        const QString late = fields.value(QStringLiteral("late_cancel")).toString();
        if (late != QLatin1String("allow") && late != QLatin1String("block"))
            return QStringLiteral("Отмена позже срока — разрешить или запретить");
    }
    if (fields.value(QStringLiteral("policy_text")).toString().trimmed().toUcs4().size() > 500)
        return QStringLiteral("Текст политики отмены — до 500 символов");
    // F5: предложения пакетов — не больше 5.
    const QVariantList offers = fields.value(QStringLiteral("packages")).toList();
    if (offers.size() > 5) return QStringLiteral("Пакетов на странице — не больше 5");
    for (const QVariant& item : offers) {
        const QVariantMap offer = item.toMap();
        const QString offerTitle = offer.value(QStringLiteral("title")).toString().trimmed();
        if (offerTitle.isEmpty() || offerTitle.toUcs4().size() > 80 || offerTitle.contains(QLatin1Char('\n')))
            return QStringLiteral("Название пакета — от 1 до 80 символов в одну строку");
        const int sessions = offer.value(QStringLiteral("sessions")).toInt();
        if (sessions < 2 || sessions > 100) return QStringLiteral("В пакете — от 2 до 100 занятий");
        const qint64 offerPrice = offer.value(QStringLiteral("price")).toLongLong();
        if (offerPrice <= 0 || offerPrice > 100000000) return QStringLiteral("Цена пакета — больше нуля");
        const int days = offer.value(QStringLiteral("valid_days"), 90).toInt();
        if (days < 7 || days > 365) return QStringLiteral("Срок пакета — от 7 до 365 дней");
    }
    return QString();
}

}  // namespace

void AppStore::loadBooking() {
    client_->sendRequest(QStringLiteral("booking.pageList"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             bookingPages_ = response.value(QStringLiteral("payload")).toObject()
                                                 .value(QStringLiteral("pages")).toArray().toVariantList();
                             bookingLoaded_ = true;
                             emit bookingChanged();
                             loadFreeSlots();  // окна первой открытой страницы — для «Сегодня»
                         });
    QJsonObject payload;
    payload.insert(QStringLiteral("limit"), 100);
    client_->sendRequest(QStringLiteral("booking.list"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             guestBookings_ = response.value(QStringLiteral("payload")).toObject()
                                                  .value(QStringLiteral("bookings")).toArray().toVariantList();
                             emit bookingChanged();
                         });
}

void AppStore::saveBookingPage(qint64 pageId, const QVariantMap& fields) {
    // Запрос уже в полёте — повторный клик не создаёт вторую страницу.
    if (bookingSaving_) return;
    const QString problem = bookingFieldsProblem(fields);
    if (!problem.isEmpty()) {
        emit bookingPageFailed(problem);
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("title"), fields.value(QStringLiteral("title")).toString().trimmed());
    payload.insert(QStringLiteral("description"), fields.value(QStringLiteral("description")).toString().trimmed());
    for (const char* key : {"duration_minutes", "days_ahead", "max_per_day", "notice_hours", "max_series"}) {
        payload.insert(QLatin1String(key), fields.value(QLatin1String(key)).toInt());
    }
    payload.insert(QStringLiteral("weekends"), fields.value(QStringLiteral("weekends")).toBool());
    // F1/F3/F4: напоминания, подпись, цена и политика — только переданные поля.
    for (const char* key : {"guest_reminders", "hide_branding"}) {
        if (fields.contains(QLatin1String(key))) payload.insert(QLatin1String(key), fields.value(QLatin1String(key)).toBool());
    }
    for (const char* key : {"price", "deposit"}) {
        if (fields.contains(QLatin1String(key)))
            payload.insert(QLatin1String(key), static_cast<double>(fields.value(QLatin1String(key)).toLongLong()));
    }
    for (const char* key : {"pay_within_minutes", "cancel_hours"}) {
        if (fields.contains(QLatin1String(key))) payload.insert(QLatin1String(key), fields.value(QLatin1String(key)).toInt());
    }
    for (const char* key : {"currency", "prepay", "payment_url", "payment_note", "late_cancel", "policy_text"}) {
        if (fields.contains(QLatin1String(key)))
            payload.insert(QLatin1String(key), fields.value(QLatin1String(key)).toString().trimmed());
    }
    // F5: предложения пакетов — целым массивом; id сохраняется у прежних.
    if (fields.contains(QStringLiteral("packages"))) {
        QJsonArray offers;
        for (const QVariant& item : fields.value(QStringLiteral("packages")).toList()) {
            const QVariantMap offer = item.toMap();
            QJsonObject entry;
            const qint64 id = offer.value(QStringLiteral("id")).toLongLong();
            if (id > 0) entry.insert(QStringLiteral("id"), static_cast<double>(id));
            entry.insert(QStringLiteral("title"), offer.value(QStringLiteral("title")).toString().trimmed());
            entry.insert(QStringLiteral("sessions"), offer.value(QStringLiteral("sessions")).toInt());
            entry.insert(QStringLiteral("price"), static_cast<double>(offer.value(QStringLiteral("price")).toLongLong()));
            entry.insert(QStringLiteral("valid_days"), offer.value(QStringLiteral("valid_days"), 90).toInt());
            offers.append(entry);
        }
        payload.insert(QStringLiteral("packages"), offers);
    }
    if (pageId > 0) payload.insert(QStringLiteral("page_id"), static_cast<double>(pageId));
    bookingSaving_ = true;
    emit bookingSavingChanged();
    client_->sendRequest(pageId > 0 ? QStringLiteral("booking.pageUpdate") : QStringLiteral("booking.pageCreate"),
                         payload, [this](const QJsonObject&, const QString& error) {
                             bookingSaving_ = false;
                             emit bookingSavingChanged();
                             if (!error.isEmpty()) {
                                 emit bookingPageFailed(explainError(error));
                                 return;
                             }
                             emit bookingPageSaved();
                             loadBooking();
                         });
}

void AppStore::setBookingPagePaused(qint64 pageId, bool paused) {
    QJsonObject payload;
    payload.insert(QStringLiteral("page_id"), static_cast<double>(pageId));
    payload.insert(QStringLiteral("status"), paused ? QStringLiteral("paused") : QStringLiteral("active"));
    client_->sendRequest(QStringLiteral("booking.pageUpdate"), payload,
                         [this, paused](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(paused ? QStringLiteral("Запись на паузе — новые гости не смогут записаться")
                                              : QStringLiteral("Запись снова открыта"));
                             loadBooking();
                         });
}

void AppStore::deleteBookingPage(qint64 pageId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("page_id"), static_cast<double>(pageId));
    client_->sendRequest(QStringLiteral("booking.pageDelete"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Страница удалена. Уже сделанные записи остаются в силе."));
                             loadBooking();
                         });
}

void AppStore::cancelGuestBooking(qint64 bookingId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("booking_id"), static_cast<double>(bookingId));
    client_->sendRequest(QStringLiteral("booking.ownerCancel"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Запись отменена"));
                             loadBooking();
                             loadTasks();
                         });
}

void AppStore::loadReferral() {
    client_->sendRequest(QStringLiteral("referral.get"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             referral_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             const QVariantMap plan = referral_.value(QStringLiteral("plan")).toMap();
                             if (!plan.isEmpty() && plan != plan_) {
                                 plan_ = plan;
                                 emit profileChanged();
                             }
                             emit referralChanged();
                         });
}

void AppStore::copyText(const QString& text) {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
        setStatus(QStringLiteral("Скопировано"));
    }
}

void AppStore::loadPermissions() {
    client_->sendRequest(QStringLiteral("permissions.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             toolPermissions_ = response.value(QStringLiteral("payload")).toObject()
                                                    .value(QStringLiteral("tools")).toArray().toVariantList();
                             emit toolPermissionsChanged();
                         });
}

void AppStore::setToolPermission(const QString& tool, const QString& mode) {
    QJsonObject payload;
    payload.insert(QStringLiteral("tool"), tool);
    payload.insert(QStringLiteral("mode"), mode);
    client_->sendRequest(QStringLiteral("permissions.set"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             // Перечитываем каталог: эффективный режим мог измениться.
                             loadPermissions();
                         });
}

void AppStore::loadConfirmations() {
    client_->sendRequest(QStringLiteral("confirmation.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             confirmations_ = response.value(QStringLiteral("payload")).toObject()
                                                  .value(QStringLiteral("actions")).toArray().toVariantList();
                             emit confirmationsChanged();
                         });
}

void AppStore::approveConfirmation(qint64 actionId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(actionId));
    client_->sendRequest(QStringLiteral("confirmation.approve"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                             setStatus(body.value(QStringLiteral("status")).toString() == QLatin1String("executed")
                                           ? QStringLiteral("Действие выполнено")
                                           : QStringLiteral("Действие не удалось выполнить"));
                             loadConfirmations();
                             loadTasks();
                             loadMemory();
                         });
}

void AppStore::denyConfirmation(qint64 actionId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(actionId));
    client_->sendRequest(QStringLiteral("confirmation.deny"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Действие отклонено"));
                             loadConfirmations();
                         });
}

void AppStore::loadTasks() {
    client_->sendRequest(QStringLiteral("tasks.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             tasks_ = response.value(QStringLiteral("payload")).toObject()
                                          .value(QStringLiteral("tasks")).toArray().toVariantList();
                             emit tasksChanged();
                         });
}

void AppStore::createTask(const QString& title, const QString& notes, const QString& remindAt) {
    if (title.trimmed().isEmpty()) {
        setError(QStringLiteral("Введите название задачи"));
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("title"), title.trimmed());
    if (!notes.trimmed().isEmpty()) payload.insert(QStringLiteral("notes"), notes.trimmed());
    if (!remindAt.trimmed().isEmpty()) payload.insert(QStringLiteral("remind_at"), remindAt.trimmed());
    client_->sendRequest(QStringLiteral("tasks.create"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Задача добавлена"));
                             loadTasks();
                         });
}

void AppStore::completeTask(qint64 taskId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(taskId));
    client_->sendRequest(QStringLiteral("tasks.complete"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             loadTasks();
                         });
}

void AppStore::cancelTask(qint64 taskId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(taskId));
    client_->sendRequest(QStringLiteral("tasks.cancel"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             loadTasks();
                         });
}

void AppStore::deleteTask(qint64 taskId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(taskId));
    client_->sendRequest(QStringLiteral("tasks.delete"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             loadTasks();
                         });
}

// ------------------------------------------------------- интеграции (этап 9)

void AppStore::loadIntegrations() {
    client_->sendRequest(QStringLiteral("integrations.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject payload =
                                 response.value(QStringLiteral("payload")).toObject();
                             integrationProviders_ = payload.value(QStringLiteral("providers"))
                                                         .toArray().toVariantList();
                             integrations_ = payload.value(QStringLiteral("connections"))
                                                 .toArray().toVariantList();
                             emit integrationsChanged();
                         });
}

bool AppStore::ensureRedirectServer() {
    if (redirectServer_ && redirectServer_->isListening()) return true;
    if (!redirectServer_) {
        redirectServer_ = new QTcpServer(this);
        connect(redirectServer_, &QTcpServer::newConnection,
                this, &AppStore::handleRedirectRequest);
    }
    if (!redirectServer_->listen(QHostAddress::LocalHost, 0)) {
        setError(QStringLiteral("Не удалось открыть локальный порт для OAuth-редиректа"));
        return false;
    }
    return true;
}

void AppStore::beginIntegration(const QString& provider) {
    if (!ensureRedirectServer()) return;
    const QString redirectUri = QStringLiteral("http://127.0.0.1:%1/callback")
                                    .arg(redirectServer_->serverPort());
    QJsonObject payload;
    payload.insert(QStringLiteral("provider"), provider);
    payload.insert(QStringLiteral("redirect_uri"), redirectUri);
    client_->sendRequest(QStringLiteral("integrations.begin"), payload,
                         [this, provider](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             integrationUrl_ = response.value(QStringLiteral("payload"))
                                                   .toObject()
                                                   .value(QStringLiteral("authorize_url"))
                                                   .toString();
                             pendingProvider_ = provider;
                             emit integrationUrlChanged();
                             if (QDesktopServices::openUrl(QUrl(integrationUrl_))) {
                                 setStatus(QStringLiteral("Открыт браузер: разрешите Selfvoy доступ"));
                             } else {
                                 setError(QStringLiteral("Не удалось открыть браузер. Ссылка: ") +
                                          integrationUrl_);
                             }
                         });
}

void AppStore::beginOAuth(const QString& provider) {
    if (!ensureRedirectServer()) return;
    pendingIsOAuthLogin_ = true;
    const QString redirectUri = QStringLiteral("http://127.0.0.1:%1/callback")
                                    .arg(redirectServer_->serverPort());
    QJsonObject payload;
    payload.insert(QStringLiteral("provider"), provider);
    payload.insert(QStringLiteral("redirect_uri"), redirectUri);
    client_->sendRequest(QStringLiteral("auth.oauth.begin"), payload,
                         [this, provider](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 pendingIsOAuthLogin_ = false;
                                 setError(error);
                                 return;
                             }
                             integrationUrl_ = response.value(QStringLiteral("payload"))
                                                   .toObject()
                                                   .value(QStringLiteral("authorize_url"))
                                                   .toString();
                             pendingProvider_ = provider;
                             emit integrationUrlChanged();
                             if (QDesktopServices::openUrl(QUrl(integrationUrl_))) {
                                 setStatus(QStringLiteral("Открыт браузер для входа: выберите аккаунт"));
                             } else {
                                 setError(QStringLiteral("Не удалось открыть браузер. Ссылка: ") +
                                          integrationUrl_);
                             }
                         });
}

void AppStore::handleRedirectRequest() {
    while (redirectServer_ && redirectServer_->hasPendingConnections()) {
        QTcpSocket* socket = redirectServer_->nextPendingConnection();
        if (!socket) continue;
        socket->waitForReadyRead(500);
        const QByteArray request = socket->readAll();
        const QByteArray firstLine = request.left(request.indexOf("\r\n"));
        // Первая строка: «GET /callback?code=...&state=... HTTP/1.1».
        QUrlQuery query;
        const int question = firstLine.indexOf('?');
        if (question >= 0) {
            const int space = firstLine.indexOf(' ', question);
            const int end = space > question ? space : firstLine.size();
            query.setQuery(QString::fromLatin1(firstLine.mid(question + 1, end - question - 1)));
        }
        const QByteArray body =
            "<html><body style='font-family:sans-serif'><h3>Selfvoy: авторизация успешна</h3>"
            "<p>Окно можно закрыть и вернуться в приложение.</p></body></html>";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n\r\n" + body);
        socket->waitForBytesWritten(500);
        socket->disconnectFromHost();
        const QString code = query.queryItemValue(QStringLiteral("code"));
        const QString state = query.queryItemValue(QStringLiteral("state"));
        if (!code.isEmpty() && !state.isEmpty() && !pendingProvider_.isEmpty()) {
            if (pendingIsOAuthLogin_) {
                pendingIsOAuthLogin_ = false;
                const QString provider = pendingProvider_;
                pendingProvider_.clear();
                QJsonObject payload;
                payload.insert(QStringLiteral("provider"), provider);
                payload.insert(QStringLiteral("code"), code);
                payload.insert(QStringLiteral("state"), state);
                client_->sendRequest(QStringLiteral("auth.oauth.callback"), payload,
                                     [this](const QJsonObject& response, const QString& error) {
                                         if (!error.isEmpty()) {
                                             setError(error);
                                             return;
                                         }
                                         applyAuth(response.value(QStringLiteral("payload")).toObject());
                                         loadChats();
                                         loadPreferences();
                                         loadMemory();
                                         loadTwoFactorStatus();
                                         setStatus(QStringLiteral("Успешный вход через OAuth"));
                                     });
            } else {
                completeIntegration(pendingProvider_, code, state);
            }
        }
    }
}

void AppStore::completeIntegration(const QString& provider, const QString& code,
                                   const QString& state) {
    QJsonObject payload;
    payload.insert(QStringLiteral("provider"), provider);
    payload.insert(QStringLiteral("code"), code);
    payload.insert(QStringLiteral("state"), state);
    client_->sendRequest(QStringLiteral("integrations.callback"), payload,
                         [this, provider](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QString account = response.value(QStringLiteral("payload"))
                                                         .toObject()
                                                         .value(QStringLiteral("account"))
                                                         .toString();
                             setStatus(account.isEmpty()
                                           ? QStringLiteral("Подключено: %1").arg(provider)
                                           : QStringLiteral("Подключено: %1 (%2)")
                                                 .arg(provider, account));
                             pendingProvider_.clear();
                             loadIntegrations();
                         });
}

void AppStore::revokeIntegration(qint64 connectionId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(connectionId));
    client_->sendRequest(QStringLiteral("integrations.revoke"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Доступ отозван"));
                             loadIntegrations();
                         });
}

void AppStore::syncIntegration(qint64 connectionId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(connectionId));
    client_->sendRequest(QStringLiteral("integrations.sync"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject payload =
                                 response.value(QStringLiteral("payload")).toObject();
                             if (payload.value(QStringLiteral("provider")).toString() ==
                                 QLatin1String("google_calendar")) {
                                 setStatus(QStringLiteral("Календарь синхронизирован: %1 событий")
                                               .arg(payload.value(QStringLiteral("events")).toInt()));
                                 refreshMemory();
                             } else {
                                 setStatus(QStringLiteral("Синхронизировано: %1")
                                               .arg(payload.value(QStringLiteral("profile_email"))
                                                        .toString()));
                             }
                         });
}

// ---------------------------------------------------------------------------
// Этап 13: уведомления и push-устройства
// ---------------------------------------------------------------------------

void AppStore::loadNotifications() {
    client_->sendRequest(QStringLiteral("notifications.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject payload =
                                 response.value(QStringLiteral("payload")).toObject();
                             notifications_ = payload.value(QStringLiteral("notifications"))
                                                  .toArray().toVariantList();
                             unreadNotifications_ = payload.value(QStringLiteral("unread")).toInt();
                             emit notificationsChanged();
                         });
}

void AppStore::markNotificationsRead(qint64 notificationId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(notificationId));
    client_->sendRequest(QStringLiteral("notifications.read"), payload,
                         [this, notificationId](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             // Локально гасим «непрочитанность», сервер даёт новый счётчик.
                             for (auto& item : notifications_) {
                                 QVariantMap map = item.toMap();
                                 if (notificationId == 0 ||
                                     static_cast<qint64>(map.value(QStringLiteral("id")).toDouble()) ==
                                         notificationId) {
                                     map.insert(QStringLiteral("read"), true);
                                     item = map;
                                 }
                             }
                             const QJsonObject result =
                                 response.value(QStringLiteral("payload")).toObject();
                             unreadNotifications_ = result.value(QStringLiteral("unread")).toInt();
                             emit notificationsChanged();
                         });
}

void AppStore::loadPushDevices() {
    client_->sendRequest(QStringLiteral("devices.push.list"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QJsonObject payload =
                                 response.value(QStringLiteral("payload")).toObject();
                             pushDevices_ = payload.value(QStringLiteral("devices"))
                                                .toArray().toVariantList();
                             emit pushDevicesChanged();
                         });
}

void AppStore::revokePushDevice(qint64 deviceId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(deviceId));
    client_->sendRequest(QStringLiteral("devices.push.revoke"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Push-устройство отозвано"));
                             loadPushDevices();
                         });
}

// ============================================================ v3: диалог, бронь, люди
void AppStore::appendDialog(QVariantMap entry) {
    entry.insert(QStringLiteral("id"), ++dialogSeq_);
    entry.insert(QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    selfvoyDialog_.append(entry);
    emit selfvoyDialogChanged();
}

void AppStore::setPendingFocus(const QString& text) {
    QSettings settings;
    if (text.trimmed().isEmpty()) settings.remove(QLatin1String(kPendingFocusKey));
    else settings.setValue(QLatin1String(kPendingFocusKey), text.trimmed());
}

void AppStore::clearSelfvoyDialog() {
    selfvoyDialog_.clear();
    emit selfvoyDialogChanged();
}

QString AppStore::toolMode(const QString& tool) const {
    for (const QVariant& item : toolPermissions_) {
        const QVariantMap entry = item.toMap();
        if (entry.value(QStringLiteral("tool")).toString() == tool) {
            return entry.value(QStringLiteral("mode")).toString();
        }
    }
    return QString();
}

void AppStore::bookNegotiation() {
    const QVariantMap slot = negotiation_.value(QStringLiteral("slot")).toMap();
    const QVariantMap place = negotiation_.value(QStringLiteral("place")).toMap();
    const QString placeName = place.value(QStringLiteral("name")).toString();
    if (!negotiation_.value(QStringLiteral("accepted")).toBool() || placeName.isEmpty()) {
        setError(QStringLiteral("Нет согласованного места для брони"));
        return;
    }
    // Пользователь запретил Selfvoy бронировать — не обходим его выбор.
    if (toolMode(QStringLiteral("book_table")) == QLatin1String("deny")) {
        setError(QStringLiteral("Бронирование запрещено в настройках разрешений"));
        return;
    }
    bookingBusy_ = true;
    emit negotiationChanged();

    QJsonObject args;
    args.insert(QStringLiteral("place"), placeName);
    args.insert(QStringLiteral("at"), slot.value(QStringLiteral("start")).toString());
    args.insert(QStringLiteral("people"), 2);
    QJsonObject payload;
    payload.insert(QStringLiteral("tool"), QStringLiteral("book_table"));
    payload.insert(QStringLiteral("args"), args);

    client_->sendRequest(QStringLiteral("tool.run"), payload,
                         [this, slot, placeName](const QJsonObject& response, const QString& error) {
                             bookingBusy_ = false;
                             if (!error.isEmpty()) {
                                 emit negotiationChanged();
                                 setError(error);
                                 return;
                             }
                             const QJsonObject booking = response.value(QStringLiteral("payload")).toObject();
                             if (booking.value(QStringLiteral("requires_confirmation")).toBool()) {
                                 // Режим «ask»: сервер ничего не забронировал, а создал отложенное
                                 // действие. Встречу в задачи не ставим — ждём подтверждения.
                                 emit negotiationChanged();
                                 loadConfirmations();
                                 setStatus(QStringLiteral("Бронирование ждёт вашего подтверждения"));
                                 return;
                             }
                             negotiation_.insert(QStringLiteral("booking"), booking.toVariantMap());
                             if (!negotiationHistory_.isEmpty()) negotiationHistory_[0] = negotiation_;
                             emit negotiationChanged();

                             // Встреча — в задачи, с напоминанием за полчаса.
                             const QDateTime start = QDateTime::fromString(
                                 slot.value(QStringLiteral("start")).toString(), Qt::ISODate);
                             const QString topic = negotiation_.value(QStringLiteral("topic")).toString();
                             createTaskAt(QStringLiteral("Встреча: %1 — %2")
                                              .arg(topic.isEmpty() ? QStringLiteral("переговоры") : topic, placeName),
                                          start.isValid() ? start.toUTC().toString(Qt::ISODate) : QString(),
                                          start.isValid() ? start.addSecs(-30 * 60).toUTC().toString(Qt::ISODate)
                                                          : QString());
                             setStatus(QStringLiteral("Столик забронирован: %1").arg(placeName));
                         });
}

void AppStore::searchUsers(const QString& query) {
    if (query.trimmed().length() < 2) {
        userResults_.clear();
        emit userResultsChanged();
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("query"), query.trimmed());
    payload.insert(QStringLiteral("limit"), 10);
    client_->sendRequest(QStringLiteral("users.search"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             userResults_ = response.value(QStringLiteral("payload")).toObject()
                                                .value(QStringLiteral("users")).toArray().toVariantList();
                             emit userResultsChanged();
                         });
}

void AppStore::createTaskAt(const QString& title, const QString& dueAt, const QString& remindAt) {
    if (title.trimmed().isEmpty()) {
        setError(QStringLiteral("Введите название задачи"));
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("title"), title.trimmed());
    if (!dueAt.trimmed().isEmpty()) payload.insert(QStringLiteral("due_at"), dueAt.trimmed());
    if (!remindAt.trimmed().isEmpty()) payload.insert(QStringLiteral("remind_at"), remindAt.trimmed());
    client_->sendRequest(QStringLiteral("tasks.create"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Задача добавлена"));
                             loadTasks();
                         });
}

void AppStore::reopenTask(qint64 taskId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("id"), static_cast<double>(taskId));
    client_->sendRequest(QStringLiteral("tasks.reopen"), payload,
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             loadTasks();
                         });
}

// ============================================================ 2026: функции роста F1–F6
// Сторона владельца: оплата записей, абонементы, ЮKassa, Telegram, ICS-подписка,
// пост со свободными окнами, тариф. Протокол — docs/PROTOCOL.md (F1–F6).

namespace {
// Текст для pro_required: как получить Pro (реферальная награда — месяц обоим).
const char* kProRequiredText =
    "Это доступно на тарифе Pro. Пригласите коллегу в Selfvoy — вы оба получите месяц Pro "
    "(Настройки → Пригласить друзей).";

double idValue(qint64 id) { return static_cast<double>(id); }
}  // namespace

QString AppStore::explainError(const QString& error) {
    // WebSocketClient отдаёт ошибку как "<code>: <message>".
    if (error.startsWith(QLatin1String("pro_required"))) {
        const QString text = QString::fromUtf8(kProRequiredText);
        emit proRequired(text);
        return text;
    }
    return error;
}

void AppStore::putBooking(const QJsonObject& booking) {
    if (booking.isEmpty()) return;
    const QVariantMap map = booking.toVariantMap();
    const qint64 id = static_cast<qint64>(booking.value(QStringLiteral("id")).toDouble());
    for (auto& item : guestBookings_) {
        if (static_cast<qint64>(item.toMap().value(QStringLiteral("id")).toDouble()) == id) {
            item = map;
            break;
        }
    }
    emit bookingChanged();
    emit bookingUpdated(map);
}

void AppStore::putPackage(const QJsonObject& package) {
    if (package.isEmpty()) return;
    const qint64 id = static_cast<qint64>(package.value(QStringLiteral("id")).toDouble());
    bool found = false;
    for (auto& item : packages_) {
        if (static_cast<qint64>(item.toMap().value(QStringLiteral("id")).toDouble()) == id) {
            item = package.toVariantMap();
            found = true;
            break;
        }
    }
    if (!found) packages_.prepend(package.toVariantMap());
    emit packagesChanged();
}

void AppStore::loadServerInfo() {
    client_->sendRequest(QStringLiteral("server.info"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             const QString bot = response.value(QStringLiteral("payload")).toObject()
                                                     .value(QStringLiteral("telegram_bot")).toString();
                             if (bot == telegramBot_) return;
                             telegramBot_ = bot;
                             emit serverInfoChanged();
                         });
}

void AppStore::refreshProfile() {
    if (userId_ == 0) return;
    client_->sendRequest(QStringLiteral("auth.me"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             const QJsonObject user = response.value(QStringLiteral("payload")).toObject();
                             plan_ = user.value(QStringLiteral("plan")).toObject().toVariantMap();
                             emit profileChanged();
                         });
}

void AppStore::loadFreeSlots() {
    if (userId_ == 0) return;
    // Окна — те же, что видит гость: booking.get по slug первой открытой страницы.
    QString slug;
    qint64 pageId = 0;
    for (const QVariant& item : bookingPages_) {
        const QVariantMap page = item.toMap();
        if (page.value(QStringLiteral("status")).toString() == QLatin1String("active")) {
            slug = page.value(QStringLiteral("slug")).toString();
            pageId = page.value(QStringLiteral("id")).toLongLong();
            break;
        }
    }
    if (slug.isEmpty()) {
        if (!freeSlots_.isEmpty()) {
            freeSlots_.clear();
            emit freeSlotsChanged();
        }
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("slug"), slug);
    client_->sendRequest(QStringLiteral("booking.get"), payload,
                         [this, pageId](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;  // окна — подсказка, ошибку не показываем
                             const QJsonArray list = response.value(QStringLiteral("payload")).toObject()
                                                          .value(QStringLiteral("page")).toObject()
                                                          .value(QStringLiteral("slots")).toArray();
                             freeSlots_.clear();
                             for (const auto& item : list) {
                                 QVariantMap map = item.toObject().toVariantMap();
                                 map.insert(QStringLiteral("page_id"), pageId);
                                 freeSlots_.append(map);
                             }
                             emit freeSlotsChanged();
                         });
}

void AppStore::setBookingPayment(qint64 bookingId, const QString& status) {
    if (status != QLatin1String("paid") && status != QLatin1String("waived") && status != QLatin1String("refunded")) return;
    QJsonObject payload;
    payload.insert(QStringLiteral("booking_id"), idValue(bookingId));
    payload.insert(QStringLiteral("status"), status);
    client_->sendRequest(QStringLiteral("booking.paymentSet"), payload,
                         [this, status](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             putBooking(response.value(QStringLiteral("payload")).toObject()
                                            .value(QStringLiteral("booking")).toObject());
                             setStatus(status == QLatin1String("paid")     ? QStringLiteral("Оплата отмечена — гостю ушло письмо")
                                       : status == QLatin1String("waived") ? QStringLiteral("Отмечено: без оплаты")
                                                                           : QStringLiteral("Отмечен возврат оплаты"));
                         });
}

void AppStore::cancelBookingOccurrence(qint64 bookingId, const QString& occurrence) {
    QJsonObject payload;
    payload.insert(QStringLiteral("booking_id"), idValue(bookingId));
    if (!occurrence.isEmpty()) payload.insert(QStringLiteral("occurrence"), occurrence);
    client_->sendRequest(QStringLiteral("booking.ownerCancel"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             putBooking(response.value(QStringLiteral("payload")).toObject()
                                            .value(QStringLiteral("booking")).toObject());
                             setStatus(QStringLiteral("Встреча отменена — гость получит письмо"));
                             loadTasks();
                             loadFreeSlots();
                         });
}

// ---------------------------------------------------------- абонементы (F5)
void AppStore::loadPackages(const QString& status) {
    packagesFilter_ = status;
    QJsonObject payload;
    payload.insert(QStringLiteral("limit"), 200);
    if (!status.isEmpty()) payload.insert(QStringLiteral("status"), status);
    client_->sendRequest(QStringLiteral("package.list"), payload,
                         [this, status](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             if (status != packagesFilter_) return;  // фильтр уже сменили
                             packages_ = response.value(QStringLiteral("payload")).toObject()
                                             .value(QStringLiteral("packages")).toArray().toVariantList();
                             packagesLoaded_ = true;
                             emit packagesChanged();
                         });
}

void AppStore::issuePackage(const QVariantMap& fields) {
    const qint64 pageId = fields.value(QStringLiteral("page_id")).toLongLong();
    const qint64 offerId = fields.value(QStringLiteral("offer_id")).toLongLong();
    const QString guest = fields.value(QStringLiteral("guest_name")).toString().trimmed();
    if (pageId <= 0) {
        emit packageFailed(QStringLiteral("Выберите страницу записи"));
        return;
    }
    if (guest.isEmpty() || guest.toUcs4().size() > 80) {
        emit packageFailed(QStringLiteral("Имя клиента — от 1 до 80 символов"));
        return;
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("page_id"), idValue(pageId));
    payload.insert(QStringLiteral("guest_name"), guest);
    if (offerId > 0) {
        payload.insert(QStringLiteral("offer_id"), idValue(offerId));
    } else {
        const QString title = fields.value(QStringLiteral("title")).toString().trimmed();
        const int sessions = fields.value(QStringLiteral("sessions")).toInt();
        const int days = fields.value(QStringLiteral("valid_days"), 90).toInt();
        const qint64 price = fields.value(QStringLiteral("price")).toLongLong();
        if (title.isEmpty() || title.toUcs4().size() > 80 || title.contains(QLatin1Char('\n'))) {
            emit packageFailed(QStringLiteral("Название — от 1 до 80 символов в одну строку"));
            return;
        }
        if (sessions < 1 || sessions > 100) {
            emit packageFailed(QStringLiteral("Занятий — от 1 до 100"));
            return;
        }
        if (days < 7 || days > 365) {
            emit packageFailed(QStringLiteral("Срок — от 7 до 365 дней"));
            return;
        }
        if (price < 0 || price > 100000000) {
            emit packageFailed(QStringLiteral("Цена — от 0 до 1 000 000"));
            return;
        }
        payload.insert(QStringLiteral("title"), title);
        payload.insert(QStringLiteral("sessions"), sessions);
        payload.insert(QStringLiteral("valid_days"), days);
        payload.insert(QStringLiteral("price"), static_cast<double>(price));
    }
    for (const char* key : {"guest_email", "cc_email"}) {
        const QString value = fields.value(QLatin1String(key)).toString().trimmed();
        if (!value.isEmpty()) payload.insert(QLatin1String(key), value);
    }
    payload.insert(QStringLiteral("paid"), fields.value(QStringLiteral("paid")).toBool());
    client_->sendRequest(QStringLiteral("package.issue"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 emit packageFailed(explainError(error));
                                 return;
                             }
                             const QJsonObject result = response.value(QStringLiteral("payload")).toObject();
                             putPackage(result.value(QStringLiteral("package")).toObject());
                             setStatus(QStringLiteral("Абонемент выдан — клиенту ушло письмо со ссылкой"));
                             emit packageIssued(result.toVariantMap());
                         });
}

void AppStore::adjustPackage(qint64 packageId, int delta) {
    if (delta == 0 || delta < -100 || delta > 100) return;
    QJsonObject payload;
    payload.insert(QStringLiteral("package_id"), idValue(packageId));
    payload.insert(QStringLiteral("delta"), delta);
    client_->sendRequest(QStringLiteral("package.adjust"), payload,
                         [this, delta](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             putPackage(response.value(QStringLiteral("payload")).toObject()
                                            .value(QStringLiteral("package")).toObject());
                             setStatus(delta > 0 ? QStringLiteral("Занятия добавлены")
                                                 : QStringLiteral("Занятия списаны"));
                         });
}

void AppStore::cancelPackage(qint64 packageId) {
    QJsonObject payload;
    payload.insert(QStringLiteral("package_id"), idValue(packageId));
    client_->sendRequest(QStringLiteral("package.cancel"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             putPackage(response.value(QStringLiteral("payload")).toObject()
                                            .value(QStringLiteral("package")).toObject());
                             setStatus(QStringLiteral("Абонемент отменён — сделанные записи остаются"));
                         });
}

void AppStore::setPackagePayment(qint64 packageId, const QString& status) {
    if (status != QLatin1String("paid") && status != QLatin1String("waived") && status != QLatin1String("refunded")) return;
    QJsonObject payload;
    payload.insert(QStringLiteral("package_id"), idValue(packageId));
    payload.insert(QStringLiteral("status"), status);
    client_->sendRequest(QStringLiteral("package.paymentSet"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             putPackage(response.value(QStringLiteral("payload")).toObject()
                                            .value(QStringLiteral("package")).toObject());
                             setStatus(QStringLiteral("Оплата абонемента отмечена"));
                         });
}

// ---------------------------------------------------------- оплата: ЮKassa (F4)
void AppStore::loadPayments() {
    client_->sendRequest(QStringLiteral("payments.status"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             paymentsStatus_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             emit paymentsChanged();
                         });
}

void AppStore::connectYooKassa(const QString& shopId, const QString& secretKey) {
    const QString shop = shopId.trimmed();
    const QString secret = secretKey.trimmed();
    if (shop.isEmpty() || shop.size() > 20 || !std::all_of(shop.begin(), shop.end(), [](QChar c) { return c.isDigit(); })) {
        emit paymentsFailed(QStringLiteral("shopId — только цифры, до 20"));
        return;
    }
    if (!secret.startsWith(QLatin1String("live_")) && !secret.startsWith(QLatin1String("test_"))) {
        emit paymentsFailed(QStringLiteral("Секретный ключ начинается с live_ или test_"));
        return;
    }
    if (paymentsBusy_) return;
    paymentsBusy_ = true;
    emit paymentsChanged();
    // Ключ живёт только в этом запросе: не сохраняется в QSettings, не пишется в журнал
    // и не попадает в errorMessage/statusMessage.
    QJsonObject payload;
    payload.insert(QStringLiteral("provider"), QStringLiteral("yookassa"));
    payload.insert(QStringLiteral("shop_id"), shop);
    payload.insert(QStringLiteral("secret_key"), secret);
    client_->sendRequest(QStringLiteral("payments.connect"), payload,
                         [this](const QJsonObject& response, const QString& error) {
                             paymentsBusy_ = false;
                             if (!error.isEmpty()) {
                                 emit paymentsChanged();
                                 emit paymentsFailed(explainError(error));
                                 return;
                             }
                             paymentsStatus_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             emit paymentsChanged();
                             emit paymentsConnected();
                             setStatus(QStringLiteral("ЮKassa подключена"));
                         });
}

void AppStore::disconnectPayments() {
    client_->sendRequest(QStringLiteral("payments.disconnect"), {},
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("ЮKassa отключена"));
                             loadPayments();
                         });
}

// ---------------------------------------------------------- Telegram-бот (F2)
void AppStore::loadTelegram() {
    client_->sendRequest(QStringLiteral("telegram.status"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             telegramStatus_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             emit telegramChanged();
                         });
}

void AppStore::linkTelegram() {
    client_->sendRequest(QStringLiteral("telegram.link"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             const QString url = response.value(QStringLiteral("payload")).toObject()
                                                     .value(QStringLiteral("url")).toString();
                             if (!url.startsWith(QLatin1String("https://"))) {
                                 setError(QStringLiteral("Сервер не прислал ссылку на бота"));
                                 return;
                             }
                             setStatus(QStringLiteral("Нажмите «Старт» в Telegram — ссылка действует 15 минут"));
                             emit telegramLinkReady(url);
                         });
}

void AppStore::unlinkTelegram() {
    client_->sendRequest(QStringLiteral("telegram.unlink"), {},
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             setStatus(QStringLiteral("Telegram отключён"));
                             loadTelegram();
                         });
}

void AppStore::setTelegramNotifications(bool enabled) {
    // prefs.set меняет ключи верхнего уровня целиком — шлём весь объект notifications.
    QVariantMap notifications = preferences_.value(QStringLiteral("notifications")).toMap();
    notifications.insert(QStringLiteral("telegram"), enabled);
    QJsonObject payload;
    payload.insert(QStringLiteral("notifications"), QJsonObject::fromVariantMap(notifications));
    client_->sendRequest(QStringLiteral("prefs.set"), payload,
                         [this, enabled](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 emit preferencesChanged();  // переключатель вернётся к сохранённому
                                 return;
                             }
                             const QVariant privacyNow = preferences_.value(QStringLiteral("privacy"));
                             preferences_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             if (privacyPending_ > 0) preferences_.insert(QStringLiteral("privacy"), privacyNow);
                             emit preferencesChanged();
                             setStatus(enabled ? QStringLiteral("Уведомления придут и в Telegram")
                                               : QStringLiteral("Уведомления в Telegram выключены"));
                         });
}

// ---------------------------------------------------------- ICS-подписка (F6)
void AppStore::applyCalendarFeed(const QJsonObject& payload) {
    QVariantMap feed;
    feed.insert(QStringLiteral("enabled"), payload.value(QStringLiteral("enabled")).toBool());
    QString url = payload.value(QStringLiteral("url")).toString().trimmed();
    const QString path = payload.value(QStringLiteral("path")).toString().trimmed();
    if (!url.startsWith(QLatin1String("http")) && path.startsWith(QLatin1Char('/'))) {
        // Сервер не знает свой внешний адрес: wss://host → https://host, ws://host → http://host.
        const QUrl server(serverUrlText_);
        const QString scheme = server.scheme() == QLatin1String("wss")  ? QStringLiteral("https")
                               : server.scheme() == QLatin1String("ws") ? QStringLiteral("http")
                                                                        : QString();
        if (!scheme.isEmpty() && !server.host().isEmpty()) {
            QUrl built;
            built.setScheme(scheme);
            built.setHost(server.host());
            if (server.port() > 0) built.setPort(server.port());
            built.setPath(path);
            url = built.toString();
        } else {
            url.clear();
        }
    }
    if (!url.startsWith(QLatin1String("http"))) url.clear();
    feed.insert(QStringLiteral("url"), url);
    feed.insert(QStringLiteral("webcal"), url.startsWith(QLatin1String("https://"))
                                              ? QStringLiteral("webcal://") + url.mid(8) : QString());
    calendarFeed_ = feed;
    emit calendarFeedChanged();
}

void AppStore::loadCalendarFeed() {
    client_->sendRequest(QStringLiteral("calendar.feed"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) return;
                             // Ссылку сервер показывает только при создании: сохраним уже
                             // полученную в этой сессии, если подписка по-прежнему включена.
                             const QJsonObject payload = response.value(QStringLiteral("payload")).toObject();
                             const QString known = calendarFeed_.value(QStringLiteral("url")).toString();
                             if (payload.value(QStringLiteral("enabled")).toBool() && !known.isEmpty()) return;
                             applyCalendarFeed(payload);
                         });
}

void AppStore::resetCalendarFeed() {
    client_->sendRequest(QStringLiteral("calendar.feedReset"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             applyCalendarFeed(response.value(QStringLiteral("payload")).toObject());
                             setStatus(QStringLiteral("Новая ссылка календаря готова — прежняя больше не работает"));
                         });
}

void AppStore::disableCalendarFeed() {
    client_->sendRequest(QStringLiteral("calendar.feedDisable"), {},
                         [this](const QJsonObject& response, const QString& error) {
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             applyCalendarFeed(response.value(QStringLiteral("payload")).toObject());
                             setStatus(QStringLiteral("Подписка на календарь выключена"));
                         });
}

// ---------------------------------------------------------- пост со свободными окнами (F6)
void AppStore::requestSlotsPost(qint64 pageId, int days, const QString& format) {
    const int span = qBound(1, days, 14);
    const QString kind = (format == QLatin1String("vk") || format == QLatin1String("plain")) ? format
                                                                                              : QStringLiteral("telegram");
    slotsPost_ = QVariantMap{{QStringLiteral("page_id"), pageId}, {QStringLiteral("loading"), true},
                             {QStringLiteral("text"), QString()}, {QStringLiteral("error"), QString()}};
    emit slotsPostChanged();
    QJsonObject payload;
    payload.insert(QStringLiteral("page_id"), idValue(pageId));
    payload.insert(QStringLiteral("days"), span);
    payload.insert(QStringLiteral("format"), kind);
    client_->sendRequest(QStringLiteral("booking.slotsPost"), payload,
                         [this, pageId](const QJsonObject& response, const QString& error) {
                             if (slotsPost_.value(QStringLiteral("page_id")).toLongLong() != pageId) return;
                             QVariantMap post;
                             post.insert(QStringLiteral("page_id"), pageId);
                             post.insert(QStringLiteral("loading"), false);
                             if (!error.isEmpty()) {
                                 post.insert(QStringLiteral("error"), explainError(error));
                             } else {
                                 const QJsonObject body = response.value(QStringLiteral("payload")).toObject();
                                 QString text = body.value(QStringLiteral("text")).toString();
                                 const QString url = body.value(QStringLiteral("url")).toString();
                                 post.insert(QStringLiteral("slots"), body.value(QStringLiteral("slots")).toInt());
                                 post.insert(QStringLiteral("url"), url);
                                 post.insert(QStringLiteral("text"), text);
                             }
                             slotsPost_ = post;
                             emit slotsPostChanged();
                         });
}

}  // namespace selfvoy
