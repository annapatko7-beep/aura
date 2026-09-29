// client/src/websocketclient.cpp
#include "websocketclient.h"

#include <QJsonDocument>
#include <QNetworkProxy>

namespace aura {

WebSocketClient::WebSocketClient(QObject* parent) : QObject(parent) {
    connect(&socket_, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(&socket_, &QWebSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(&socket_, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
    connect(&socket_, &QWebSocket::errorOccurred, this, &WebSocketClient::onSocketError);
}

void WebSocketClient::setUrl(const QUrl& url) { url_ = url; }

void WebSocketClient::setState(State state) {
    if (state_ == state) return;
    state_ = state;
    emit stateChanged(state_);
}

void WebSocketClient::connectToServer() {
    if (state_ != State::Disconnected) return;
    setState(State::Connecting);

    QNetworkRequest request{url_};
    if (!token_.isEmpty()) {
        // Сервер принимает JWT прямо в рукопожатии — вход без лишнего round-trip.
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(token_).toUtf8());
    }
    request.setRawHeader("User-Agent", "aura-qt-client/0.1");
    socket_.open(request);
}

void WebSocketClient::disconnectFromServer() {
    if (state_ == State::Disconnected) return;
    socket_.close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("client shutdown"));
}

QString WebSocketClient::sendRequest(const QString& type, const QJsonObject& payload, Callback callback) {
    const QString id = QStringLiteral("req-%1-%2").arg(type).arg(++counter_);

    QJsonObject message;
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("type"), type);
    message.insert(QStringLiteral("payload"), payload);

    if (!isConnected()) {
        if (callback) {
            callback(QJsonObject(), QStringLiteral("нет соединения с сервером"));
        }
        return id;
    }
    if (callback) pending_.insert(id, std::move(callback));
    socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    return id;
}

void WebSocketClient::onConnected() {
    setState(State::Connected);
}

void WebSocketClient::onDisconnected() {
    pending_.clear();
    setState(State::Disconnected);
}

void WebSocketClient::onSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    const QString message = socket_.errorString();
    emit errorOccurred(message.isEmpty() ? QStringLiteral("ошибка соединения") : message);
    setState(State::Disconnected);
}

void WebSocketClient::onTextMessageReceived(const QString& message) {
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit errorOccurred(QStringLiteral("сервер прислал не-JSON: %1").arg(parseError.errorString()));
        return;
    }

    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("event")) {
        emit eventReceived(object.value(QStringLiteral("event")).toString(),
                           object.value(QStringLiteral("payload")).toObject());
        return;
    }

    const QString id = object.value(QStringLiteral("id")).toString();
    const auto it = pending_.find(id);
    if (it == pending_.end()) return;
    const Callback callback = it.value();
    pending_.erase(it);

    if (type == QLatin1String("error")) {
        const QString code = object.value(QStringLiteral("code")).toString();
        const QString text = object.value(QStringLiteral("message")).toString();
        if (callback) callback(object, QStringLiteral("%1: %2").arg(code, text));
        return;
    }
    if (callback) callback(object, QString());
}

}  // namespace aura
