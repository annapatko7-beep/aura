// client/src/appstore.cpp
#include "appstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>
#include <QVariant>

// Голосовой ввод (STT) и озвучка (TTS): платформенные модули Qt.
// В QML их нет — вся работа с микрофоном и синтезом речи живёт здесь, в C++,
// а QML обращается только к свойствам/сигналам/слотам AppStore.
#include <QAudioDevice>
#include <QAudioSource>
#include <QMediaDevices>
#include <QTextToSpeech>
#include <QVoice>

#include <cmath>

#include "websocketclient.h"

namespace aura {

namespace {
constexpr const char* kTokenKey = "aura/token";
constexpr const char* kRefreshKey = "aura/refresh_token";
constexpr const char* kServerKey = "aura/server";
constexpr const char* kDeviceIdKey = "aura/device_id";
// Голосовой ввод и озвучка.
constexpr const char* kAutoSendVoiceKey = "aura/voice/auto_send";
constexpr const char* kMicLanguageKey = "aura/voice/language";
constexpr const char* kTtsEnabledKey = "aura/tts/enabled";
constexpr const char* kTtsVoiceKey = "aura/tts/voice";
constexpr const char* kTtsRateKey = "aura/tts/rate";
constexpr const char* kTtsVolumeKey = "aura/tts/volume";
constexpr const char* kTtsImportantOnlyKey = "aura/tts/important_only";
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
}  // namespace

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
        } else {
            setStatus(QStringLiteral("Нет соединения"));
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

void AppStore::start() {
    QSettings settings;
    token_ = settings.value(QLatin1String(kTokenKey)).toString();
    refreshToken_ = settings.value(QLatin1String(kRefreshKey)).toString();
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
    if (!client_->isConnected()) {
        return QStringLiteral("подключение к %1…").arg(serverUrlText_);
    }
    return authenticated() ? QStringLiteral("Аура на связи") : QStringLiteral("сервер: %1").arg(serverUrlText_);
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
    errorMessage_ = message;
    emit errorMessageChanged();
}

void AppStore::applyAuth(const QJsonObject& payload) {
    const QJsonObject user = payload.value(QStringLiteral("user")).toObject();
    const bool firstTime = userId_ == 0;

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

    if (payload.contains(QStringLiteral("preferences"))) {
        preferences_ = payload.value(QStringLiteral("preferences")).toObject().toVariantMap();
        emit preferencesChanged();
    }

    emit profileChanged();
    if (firstTime) emit authenticatedChanged();
    // Этап 8: сразу подтягиваем разрешения, отложенные подтверждения и задачи.
    loadPermissions();
    loadConfirmations();
    loadTasks();
    setStatus(QStringLiteral("Добро пожаловать, %1").arg(userName_));
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

void AppStore::registerUser(const QString& displayName, const QString& email, const QString& password) {
    setBusy(true);
    QJsonObject payload;
    payload.insert(QStringLiteral("display_name"), displayName);
    payload.insert(QStringLiteral("email"), email);
    payload.insert(QStringLiteral("password"), password);
    payload.insert(QStringLiteral("device"), QStringLiteral("qt-client"));

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
                             setStatus(QStringLiteral("Пароль изменён, прочие устройства отключены"));
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
    setStatus(QStringLiteral("Аура думает…"));
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
                             setStatus(reply.isEmpty() ? QStringLiteral("Аура выполнила задачу") : reply);
                             // Ответ Ауры — «важное»: озвучиваем даже в режиме «только важное».
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

void AppStore::savePreferences(const QVariantMap& preferences) {
    setBusy(true);
    client_->sendRequest(QStringLiteral("prefs.set"), QJsonObject::fromVariantMap(preferences),
                         [this](const QJsonObject& response, const QString& error) {
                             setBusy(false);
                             if (!error.isEmpty()) {
                                 setError(error);
                                 return;
                             }
                             preferences_ =
                                 response.value(QStringLiteral("payload")).toObject().toVariantMap();
                             emit preferencesChanged();
                             setStatus(QStringLiteral("Настройки Ауры сохранены"));
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
        preferences_ = response.value(QStringLiteral("payload")).toObject().toVariantMap();
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

}  // namespace aura
