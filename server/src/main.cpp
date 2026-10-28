// selfvoy/server main — точка входа C++-сервера Selfvoy.
//
//   ./selfvoy-server [--port 9000] [--host 0.0.0.0] [--help]
//
// Конфигурация — переменные окружения (см. selfvoy/config.h):
//   SELFVOY_HOST, SELFVOY_PORT, SELFVOY_DATABASE_URL, SELFVOY_AI_URL, SELFVOY_JWT_SECRET, ...
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include "selfvoy/log.h"
#include "selfvoy/server.h"

namespace {

selfvoy::Server* g_server = nullptr;

void onSignal(int signal) {
    SELFVOY_LOG(selfvoy::log::Level::Info, "main") << "получен сигнал " << signal << ", останавливаюсь";
    if (g_server != nullptr) g_server->requestStop();
}

void printUsage() {
    std::cout <<
        "Сервер Selfvoy — сеть ИИ-агентов\n"
        "\n"
        "Использование: selfvoy-server [опции]\n"
        "  --host <addr>      адрес для прослушивания (SELFVOY_HOST, по умолчанию 0.0.0.0)\n"
        "  --port <port>      порт WebSocket (SELFVOY_PORT, по умолчанию 9000)\n"
        "  --database <url>   PostgreSQL DSN (SELFVOY_DATABASE_URL)\n"
        "  --ai <url>         адрес Python AI Service (SELFVOY_AI_URL)\n"
        "  --log <level>      debug|info|warn|error (SELFVOY_LOG_LEVEL)\n"
        "  --help             эта справка\n";
}

}  // namespace

int main(int argc, char** argv) {
    selfvoy::Config config = selfvoy::Config::fromEnv();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "не хватает значения для " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--host") {
            config.host = next("--host");
        } else if (arg == "--port") {
            config.port = std::stoi(next("--port"));
        } else if (arg == "--database") {
            config.databaseUrl = next("--database");
        } else if (arg == "--ai") {
            config.aiServiceUrl = next("--ai");
        } else if (arg == "--log") {
            config.logLevel = next("--log");
        } else {
            std::cerr << "неизвестный аргумент: " << arg << "\n";
            printUsage();
            return 2;
        }
    }

    selfvoy::Server server(config);
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    std::string error;
    if (!server.start(error)) {
        std::cerr << "не удалось запустить сервер: " << error << "\n";
        return 1;
    }

    SELFVOY_LOG(selfvoy::log::Level::Info, "main")
        << "WebSocket: ws://" << config.host << ":" << server.port() << "  (Ctrl+C — остановка)";
    server.wait();
    server.stop();
    SELFVOY_LOG(selfvoy::log::Level::Info, "main") << "сервер остановлен";
    return 0;
}
