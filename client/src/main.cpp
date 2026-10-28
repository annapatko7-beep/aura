// client/src/main.cpp — точка входа Qt-клиента Selfvoy.
//
//   SELFVOY_SERVER_URL=ws://127.0.0.1:9000 ./selfvoy-client
#include <QDirIterator>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSettings>
#include <QUrl>

#include "appstore.h"

namespace {

// Однократный перенос настроек прежнего имени клиента (QSettings "Aura"/"Aura",
// ключи "aura/..."): токены, адрес сервера, device_id и голос переезжают в
// "Selfvoy"/"Selfvoy" с ключами "selfvoy/...", чтобы вход не слетел после
// обновления. Срабатывает, только пока новые настройки пусты; старые не трогаем.
void migrateLegacySettings() {
    QSettings current;
    if (!current.allKeys().isEmpty()) return;
    QSettings legacy(QStringLiteral("Aura"), QStringLiteral("Aura"));
    const QString legacyPrefix = QStringLiteral("aura/");
    const QStringList keys = legacy.allKeys();
    for (const QString& key : keys) {
        const QString newKey = key.startsWith(legacyPrefix)
                                   ? QStringLiteral("selfvoy/") + key.mid(legacyPrefix.size())
                                   : key;
        current.setValue(newKey, legacy.value(key));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("Selfvoy"));
    QGuiApplication::setApplicationName(QStringLiteral("Selfvoy"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/selfvoy.png")));
    migrateLegacySettings();

    // Manrope из ресурсов — шрифт макетов на любой системе.
    QDirIterator fonts(QStringLiteral(":/fonts"), {QStringLiteral("*.ttf")});
    while (fonts.hasNext()) QFontDatabase::addApplicationFont(fonts.next());

    // Свой дизайн полностью на QML (графит + стекло), стандартный стиль не нужен.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    selfvoy::AppStore store;
    // SELFVOY_SERVER_URL — для разработки; иначе адрес из настроек (его меняют на
    // экране входа и в настройках), по умолчанию ws://127.0.0.1:9000.
    if (qEnvironmentVariableIsSet("SELFVOY_SERVER_URL")) {
        store.setServerUrl(QUrl(qEnvironmentVariable("SELFVOY_SERVER_URL")));
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &store);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Selfvoy"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return -1;

    store.start();
    return QGuiApplication::exec();
}
