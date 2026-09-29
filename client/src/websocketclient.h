// client/src/websocketclient.h — WebSocket-клиент Aura (Qt 6, модуль QtWebSockets).
//
// Отвечает за транспорт: подключение, авторизация токеном, корреляция
// «запрос → ответ» по id и доставка push-событий (chat.message, chat.created…).
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QWebSocket>

#include <functional>

namespace aura {

class WebSocketClient : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Connected };
    Q_ENUM(State)

    explicit WebSocketClient(QObject* parent = nullptr);

    void setUrl(const QUrl& url);
    QUrl url() const { return url_; }
    void setToken(const QString& token) { token_ = token; }
    QString token() const { return token_; }

    State state() const { return state_; }
    bool isConnected() const { return state_ == State::Connected; }

    using Callback = std::function<void(const QJsonObject& response, const QString& error)>;

    // Отправляет {"id","type","payload"} и запоминает callback до ответа.
    QString sendRequest(const QString& type,
                        const QJsonObject& payload = QJsonObject(),
                        Callback callback = nullptr);

public slots:
    void connectToServer();
    void disconnectFromServer();

signals:
    void stateChanged(aura::WebSocketClient::State state);
    void eventReceived(const QString& name, const QJsonObject& payload);
    void errorOccurred(const QString& message);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onSocketError(QAbstractSocket::SocketError error);

private:
    void setState(State state);

    QWebSocket socket_;
    QUrl url_;
    QString token_;
    State state_ = State::Disconnected;
    QHash<QString, Callback> pending_;
    qint64 counter_ = 0;
};

}  // namespace aura
