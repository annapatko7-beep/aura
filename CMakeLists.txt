cmake_minimum_required(VERSION 3.21)

project(aura VERSION 0.1.0 LANGUAGES C CXX)

# Корневая сборка: сервер обязателен, Qt-клиент — по желанию
# (AURA_WITH_CLIENT=ON требует Qt 6: Core, Network, WebSockets, Quick, Qml).
option(AURA_WITH_CLIENT "Собирать Qt/QML-клиент" OFF)

add_subdirectory(server)

if(AURA_WITH_CLIENT)
    add_subdirectory(client)
endif()
