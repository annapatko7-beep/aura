// client/src/main.cpp — точка входа Qt-клиента Aura.
//
//   AURA_SERVER_URL=ws://127.0.0.1:9000 ./aura-client
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "appstore.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Aura"));
    QGuiApplication::setApplicationName(QStringLiteral("Aura"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Aura — сеть ИИ-агентов"));

    // Свой дизайн полностью на QML (графит + стекло), стандартный стиль не нужен.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    aura::AppStore store;
    const QString serverUrl =
        qEnvironmentVariable("AURA_SERVER_URL", QStringLiteral("ws://127.0.0.1:9000"));
    store.setServerUrl(QUrl(serverUrl));

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &store);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Aura"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return -1;

    store.start();
    return QGuiApplication::exec();
}
